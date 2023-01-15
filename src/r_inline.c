/* R content HTTP handler.

   (C)Copyright 2022 by Simon Urbanek <simon.urbanek@R-project.org>
   License: MIT

*/

#include "ulog.h"
#include "http.h"
#include "http_tools.h"
#include "http_date.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

#include <Rinternals.h>

typedef struct r_inline {
    char *prefix;
    SEXP sFun;
    int prefix_len;
    int flags;
} r_inline;

void r_inline_free(r_inline *obj) {
    if (!obj)
	return;
    if (obj->prefix)
	free(obj->prefix);
    if (obj->sFun && obj->sFun != R_NilValue)
	R_ReleaseObject(obj->sFun);
    free(obj);
}

r_inline *r_inline_entry(const char *prefix, SEXP sFun, int flags) {
    r_inline *r;
    if (!prefix)
	return 0;

    r = (r_inline*) calloc(1, sizeof(r_inline));
    if (!r || !(r->prefix = strdup(prefix)))
	return 0;
    r->flags = flags;
    r->prefix_len = strlen(prefix); /* cached for performance */
    r->sFun = sFun;
    if (sFun && sFun != R_NilValue)
	R_PreserveObject(sFun);
    return r;
}

typedef struct rs_eval {
    SEXP what, rho, last, traceback, handlers;
    int exp;
} rs_eval_t;

extern int R_Visible;

static SEXP R_eval_do(void *arg) {
    rs_eval_t *e = (rs_eval_t*) arg;
    SEXP what = e->what, rho = e->rho, x;
    int i, n;

    /* add calling handlers to catch error conditions so we can report them - see #154 */
    if (e->handlers) {
        SEXP sInternal = Rf_install(".Internal");
        SEXP addCondHands = Rf_install(".addCondHands");
        SEXP ach = PROTECT(lang2(sInternal,
                            lang6(addCondHands,
                                  Rf_getAttrib(e->handlers, R_NamesSymbol),
                                  e->handlers,
                                  rho, R_NilValue, PROTECT(Rf_ScalarLogical(1)))));
        eval(ach, rho);
        UNPROTECT(2);
    }

    if (TYPEOF(what) == EXPRSXP) {
        n = LENGTH(what);
        for (i = 0; i < n; i++) {
            e->exp = i;
            x = eval(VECTOR_ELT(what, i), rho);
            if (i == n - 1) {
                R_PreserveObject(x);
                e->last = x;
            }
            if (R_Visible)
                PrintValue(x);
        }
    } else {
        e->exp = -1;
        x = eval(what, rho);
        R_PreserveObject(x);
        /* intentionally we don't print if it is not an expression vector */
        e->last = x;
    }
    return R_NilValue;
}

/* it's really stupid becasue R has R_GetTraceback() but we have to
   jump through eval() just because it's hidden so we can't access it ... */
static SEXP R_GetTraceback(int skip) {
    SEXP d_int = install(".Internal"), tb = install("traceback"), sSkip = PROTECT(ScalarInteger(skip));
    SEXP what = PROTECT(lang2(d_int, lang2(tb, sSkip)));
    SEXP res = eval(what, R_GlobalEnv);
    UNPROTECT(2);
    return res;
}

static void Rserve_eval_cleanup(void *arg) {
    rs_eval_t *e = (rs_eval_t*) arg;
    SEXP tb = R_GetTraceback(0);
    if (tb && tb != R_NilValue)
        R_PreserveObject((e->traceback = tb));
}

static void Rserve_eval_(void *arg) {
    R_ExecWithCleanup(R_eval_do, arg, Rserve_eval_cleanup, arg);
}

int r_inline_handler(http_request_t *req, http_result_t *res, void *user) {
    const char *path;
    r_inline *r = (r_inline*) user;

    /* this should never happen, but just to be safe ... */
    if (!req || !req->url || !r) return 0;

    begin_pool("r_inline_handler");

    path = req->url;
    DBG(ulog_debug("r_inline: match '%s' and '%s'", path, r->prefix));
    if (strncmp(r->prefix, path, r->prefix_len)) {
	end_pool();
	return 0;
    }
    DBG(ulog_debug("r_inline: matched"));

    {
	SEXP what = PROTECT(lang2(r->sFun, mkString(req->url))), x;
	rs_eval_t e = { what, R_GlobalEnv, 0, 0, 0, 0 };
	int ter = R_ToplevelExec(Rserve_eval_, &e);
	UNPROTECT(1);
	do {
	    if (!ter || !e.last) {
		const char *errmsg = R_curErrorBuf();
		res->err = strdup(errmsg);
		break;
	    }
	    x = e.last;
	    if (TYPEOF(x) == STRSXP && LENGTH(x) > 0) { /* string means there was an error */
		res->err = strdup(CHAR(STRING_ELT(x, 0)));
		break;
	    }

	    if (TYPEOF(x) == VECSXP && LENGTH(x) > 0) { /* a list (generic vector) can be a real payload */
		SEXP xNames = getAttrib(x, R_NamesSymbol);
		if (LENGTH(x) > 1) {
		    SEXP sCT = VECTOR_ELT(x, 1); /* second element is content type if present */
		    if (TYPEOF(sCT) == STRSXP && LENGTH(sCT) > 0) {
			/* FIXME: mem mgmt of ct? */
			res->content_type = strdup(CHAR(STRING_ELT(sCT, 0)));
			if (LENGTH(x) > 2) { /* third element is headers vector */
			    //sHeaders = VECTOR_ELT(x, 2);
			    //if (TYPEOF(sHeaders) != STRSXP)
			    //    sHeaders = R_NilValue;
			    if (LENGTH(x) > 3) /* fourth element is HTTP code */
				res->code = asInteger(VECTOR_ELT(x, 3));
			}
		    }
		}
		SEXP y = VECTOR_ELT(x, 0);
		if (TYPEOF(y) == STRSXP && LENGTH(y) > 0) {
		    int  is_tmp = 0;
		    const char *cs = CHAR(STRING_ELT(y, 0));
		    if (TYPEOF(xNames) == STRSXP && LENGTH(xNames) > 0 &&
			(!strcmp(CHAR(STRING_ELT(xNames, 0)), "file") || (is_tmp = !strcmp(CHAR(STRING_ELT(xNames, 0)), "tmpfile")))) {
			res->payload = strdup(cs);
			res->payload_type = is_tmp ? PAYLOAD_TEMPFILE : PAYLOAD_FILE;
			break;
		    }
		    res->payload = strdup(cs);
		    res->payload_len = strlen(res->payload);
		    break;
		}
		if (TYPEOF(y) == RAWSXP) {
		    res->payload_len = XLENGTH(y);
		    res->payload = (char*) malloc(res->payload_len);
		    memcpy(res->payload, RAW(y), res->payload_len);
		    break;
		}
		/* not anything we recognize */
		res->err = strdup("Invalid response by R.");
	    }
	} while(0);
	if (e.last)
	    R_ReleaseObject(e.last);

	if (e.traceback)
	    R_ReleaseObject(e.traceback);
    }
    end_pool();
    return 1;
}
