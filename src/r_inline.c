/* R content HTTP handler.

   (C)Copyright 2022 by Simon Urbanek <simon.urbanek@R-project.org>
   License: MIT

*/

#include "ulog.h"
#include "http.h"
#include "http_tools.h"
#include "http_date.h"
#include "r_inline.h"
#include "async.h"
#include "r_ext.h"

#include "r_http_tools.h"

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

/* We use an (empty) queue to synchronize calls into R.
   Note that the evaluation will still happen in the 
   process thread, but the mutex guarantees that it is always
   done in a synchronized fashion. Because the calls are
   on different threads, we have to disable the R's stack
   limit checks since they won't work.
   Note that this works mainly because we know that our
   accept code is running on the main thread, which will
   never enter R.
   We could improve this by starting the server loop in
   a spearate thread and then use the r_queue to schedule
   calls into the main loop, but that would mean additonal
   thread jumping, because we still want to separate I/O
   threads. */

static async_queue *r_queue;

extern uintptr_t R_CStackLimit;

void r_inline_setup(int flags) {
    if ((flags & RIF_ASYNC) && !r_queue) {
	r_queue = async_queue_new();
	R_CStackLimit = -1;
    }
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

/* collect (unnamed) string R headers into one, freshly allocated
   char* string. Empty lines (elements) are silently removed.
   Caller owns the result (which can be NULL if there is no
   [valid] content). */
static char *R_headers2cstr(SEXP sHeaders) {
    char *res, *dst;
    size_t len = 0;
    int i = 0, n = LENGTH(sHeaders);
    while (i < n) {
	const char *c = CHAR(STRING_ELT(sHeaders, i++));
	if (*c && *c != '\r' && *c == '\n') /* empty headers are illegal */
	    len += strlen(c) + 2;
    }
    if (!len)
	return 0;
    dst = res = malloc(len + 1);
    if (!res) {
	ulog_error("unable to allocate memory for headers");
	return 0;
    }
    i = 0;
    while (i < n) {
	const char *c = CHAR(STRING_ELT(sHeaders, i++));
	if (*c && *c != '\r' && *c == '\n') {
	    size_t l = strlen(c);
	    memcpy(dst, c, l);
	    dst += l;
	    /* all header lines must be \r\n terminated */
	    if (dst[-1] != '\n') {
		*(dst++) = '\r';
		*(dst++) = '\n';
	    }
	}
    }
    *dst = 0;
    return res;
}

static SEXP buf2raw(const char *buf, size_t len) {
    if (buf && len) {
	SEXP r = allocVector(RAWSXP, len);
	memcpy(RAW(r), buf, len);
	return r;
    }
    return R_NilValue;
}

void r_inline_process(http_request_t *req, http_result_t *res, SEXP sFun, int flags) {
    /* original has: (path, query, body, headers) */
    SEXP sPath     = PROTECT(mkString(req->url));
    SEXP sQuery    = PROTECT(req->query ?
			     ((flags & RIF_PARSE_QUERY) ? r_http_parse_query(req->query) : mkString(req->query)) :
			     R_NilValue);
    SEXP sBody     = PROTECT(buf2raw(req->body, req->body_len));
    SEXP sCTS      = Rf_install("content-type");
    SEXP sHdr      = PROTECT((flags & RIF_PARSE_HEADERS) ? r_http_parse_headers(req->headers, -1) :
			     buf2raw(req->headers, req->headers ? strlen(req->headers) : 0));
    SEXP sMethod   = PROTECT(mkString(req->method));
    if (req->content_type)
	Rf_setAttrib(sBody, sCTS, mkString(req->content_type));
    /* fragments are not sent even though they are allowed, so for now let's ignore them
       SEXP sFragment = PROTECT(req->fragment ? mkString(req->fragment) : R_NilValue); */
    SEXP what      = PROTECT(LCONS(sFun, list5(sPath, sQuery, sBody, sHdr, sMethod))), x;
    rs_eval_t e = { what, R_GlobalEnv, 0, 0, 0, 0 };
    int ter = R_ToplevelExec(Rserve_eval_, &e);
    UNPROTECT(6);
    do {
	if (!ter || !e.last) {
	    /* we have (possibly) more info: e.traceback */
	    const char *errmsg = R_curErrorBuf();
	    DBG(ulog_debug("r_inline: evaluation failed with %s", errmsg));
	    res->err = strdup(errmsg);
	    break;
	}
	x = e.last;
	if (TYPEOF(x) == STRSXP && LENGTH(x) > 0) { /* string means there was an error */
	    res->err = strdup(CHAR(STRING_ELT(x, 0)));
	    break;
	}

	if (TYPEOF(x) == LGLSXP && LENGTH(x) == 1 && !asInteger(x)) { /* FALSE => did not process */
	    res->flags |= RF_REFUSED;
	    break;
	}

	if (TYPEOF(x) == VECSXP && LENGTH(x) > 0) { /* a list (generic vector) can be a real payload */
	    SEXP xNames = getAttrib(x, R_NamesSymbol);
	    if (LENGTH(x) > 1) {
		SEXP sCT = VECTOR_ELT(x, 1); /* second element is content type if present */
		if (TYPEOF(sCT) == STRSXP && LENGTH(sCT) > 0) {
		    if (res->content_type)
			free(res->content_type);
		    res->content_type = strdup(CHAR(STRING_ELT(sCT, 0)));
		    if (LENGTH(x) > 2) { /* third element is headers vector */
			SEXP sHeaders = VECTOR_ELT(x, 2);
			if (TYPEOF(sHeaders) == STRSXP && LENGTH(sHeaders) > 0)
			    res->headers = R_headers2cstr(sHeaders);
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
		    if (res->payload)
			free(res->payload);
		    res->payload = strdup(cs);
		    res->payload_type = is_tmp ? PAYLOAD_TEMPFILE : PAYLOAD_FILE;
		    break;
		}
		if (res->payload)
		    free(res->payload);
		res->payload = strdup(cs);
		res->payload_len = strlen(res->payload);
		break;
	    }
	    if (TYPEOF(y) == RAWSXP) {
		res->payload_len = XLENGTH(y);
		if (res->payload)
		    free(res->payload);
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

struct client_connection {
    /* common information for all connections */
    client_connection_info_t info;
};

static __thread client_connection_t *worker;

void r_inline_end() {
    if (r_queue) {
	async_queue_finish(r_queue);
	async_queue_free(r_queue);
	r_queue = 0;
    }
}

void r_inline_thread_end() {
    if (worker) {
	DBG(ulog_debug("r_inline_end: disposing of workers"));
	if (r_ext_ready(worker))
	    r_ext_close(worker);
	if (worker->info.srv && worker->info.srv->conn_fin)
	    worker->info.srv->conn_fin(worker);
	free(worker);
	worker = 0;
    }
}

int r_inline_handler(http_request_t *req, http_result_t *res, void *user) {
    const char *path;
    r_inline *r = (r_inline*) user;

    DBG(ulog_debug("r_inline_handler(%p [%s], %p, %p)", (void*) req,
		   (req && req->url) ? req->url : "<NULL>",
		   (void*) res, (void*) user));
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

    if (r->flags & RIF_FORK) {
	client_connection_t *old_worker = 0;
	int opr = 0;
	do {
	    /* check if the current worker is usable */
	    if (worker && !r_ext_ready(worker)) {
		if (old_worker) {
		    DBG(ulog_debug("r_inline: releasing non-functional worker %p", (void*) old_worker));
		    if (old_worker->info.srv && old_worker->info.srv->conn_fin)
			old_worker->info.srv->conn_fin(old_worker);
		    free(old_worker);
		}
		old_worker = worker;
		r_ext_close(worker);
		worker = 0;
	    }
	    if (!worker) {
		DBG(ulog_debug("r_inline: RIF_FORK requested, starting new worker"));
		r->flags ^= RIF_FORK;
		/* the worker should not have the FORK flag */
		worker = r_ext_new_worker(r);
		r->flags |= RIF_FORK;
		if (!worker) {
		    /* we delayed the destruction of the old worker for
		       performance reasons, but now we have to do it */
		    if (old_worker) {
			if (old_worker->info.srv && old_worker->info.srv->conn_fin)
			    old_worker->info.srv->conn_fin(old_worker);
			free(old_worker);
		    }
		    return -1;
		}
	    }
	    DBG(ulog_debug("r_inline: switching to r_ext worker at %p (%d)",
			   (void*) worker, (int) worker->info.s));
	    opr = r_ext_one_request(worker, req, res);
	    if (opr == -2) /* worker failed */
		DBG(ulog_debug("r_inline: worker failed - respawning"));
	} while (opr == -2);

	if (opr == -1) { /* internal error in the handler */
	    res->code = 500;
	    ulog_error("r_ext worker memory allocation error");
	    return 1;
	}

	if (old_worker) {
	    DBG(ulog_debug("r_inline: releasing non-functional worker %p", (void*) old_worker));
	    if (old_worker->info.srv && old_worker->info.srv->conn_fin)
		old_worker->info.srv->conn_fin(old_worker);
	    free(old_worker);
	}
	if (res->flags & RF_REFUSED) {
	    res->flags &= ~RF_REFUSED;
	    return 0;
	}
	return 1;
    }

    if (r_queue)
	async_queue_begin(r_queue);
    r_inline_process(req, res, r->sFun, r->flags);
    if (r_queue)
	async_queue_end(r_queue);
    end_pool();
    if (res->flags & RF_REFUSED) {
	res->flags &= ~RF_REFUSED;
	return 0;
    }
    return 1;
}
