/* HTTP and WebSocket server serving static content and
   forwarding QAP1 protocol.
   Based on Rserve.

   (C)Copyright 2014 Simon Urbanek

   License: BSD
   
*/

#ifndef NO_R

#include "http.h"
#include "tls.h"
#include "ulog.h"
#include "bsdcmpt.h"
#include "chandler.h"
#include "http_date.h"
#include "http_tools.h"
#include "http_static.h"
#include "http_rewrite.h"
#include "r_inline.h"
#include "r_http_tools.h"

#include "dalloc.h"

#include <Rinternals.h>

static void http_request(http_request_t *req, http_result_t *res) {
    DBG({
	    char date[32];
	    ulog_info("request for '%s', Date: %s, headers:\n%s\n", req->url, posix2http(req->date, date), req->headers ? req->headers : "<NONE>");
	});

    sanitize_path(req->url);
    if (!call_content_handlers(req, res)) {
        res->code = 404;
    }
}

static SEXP findArg(SEXP args, const char *name, SEXP sDefault) {
    SEXP mySym = Rf_install(name);
    while (args != R_NilValue) {
	if (TAG(args) == mySym)
	    return CAR(args);
	args = CDR(args);
    }
    return sDefault;
}

SEXP C_add_handler(SEXP args) {
    SEXP sType, sPrefix;
    const char *htype;
    args = CDR(args);
    sType = CAR(args); args = CDR(args);
    if (!isString(sType) || LENGTH(sType) != 1)
	Rf_error("handler type must be a string");
    htype = CHAR(STRING_ELT(sType, 0));
    sPrefix = CAR(args); args = CDR(args);
    if (!isString(sPrefix) || LENGTH(sPrefix) != 1)
	Rf_error("prefix must be a string");
    SEXP sStop = findArg(args, "stop", 0);
    int do_stop = sStop ? Rf_asInteger(sStop) : 0;
    if (!strcmp(htype, "static")) {
	SEXP sPath = CAR(args); args = CDR(args);
	if (!isString(sPath) || LENGTH(sPath) != 1)
	    Rf_error("path must be a string");
	SEXP sIndex = R_NilValue;
	if (TAG(args) == R_NilValue) { /* unnamed -> positional */
	    sIndex = CAR(args); args = CDR(args);
	} else
	    sIndex = findArg(args, "index", R_NilValue);
	if (sIndex != R_NilValue && (!isString(sIndex) || LENGTH(sIndex) != 1 ||
				     STRING_ELT(sIndex, 0) == NA_STRING))
	    Rf_error("index must be a string or NULL");
	SEXP sGZ = findArg(args, "gzip", 0);
	int do_gzip = sGZ ? Rf_asInteger(sGZ) : 0;
	add_content_handler(http_static_handler,
			    http_static_entry(CHAR(STRING_ELT(sPrefix, 0)),
					      CHAR(STRING_ELT(sPath, 0)),
					      (sIndex == R_NilValue) ? 0 : CHAR(STRING_ELT(sIndex, 0)),
					      (do_stop ? HSF_STOP : 0) |
					      (do_gzip ? HSF_PRECOMPRESSED : 0)),
			    NULL, (free_handler_fn_t) http_static_free);
    } else if (!strcmp(htype, "rewrite")) {
	SEXP sReplace = CAR(args); args = CDR(args);
	if (!isString(sReplace) || LENGTH(sReplace) != 1)
	    Rf_error("replacement must be a string");
	add_content_handler(http_rewrite_handler,
			    http_rewrite_entry(CHAR(STRING_ELT(sPrefix, 0)),
					       CHAR(STRING_ELT(sReplace, 0)),
					       (do_stop ? REWRITE_FLAG_LAST : 0)),
			    NULL, (free_handler_fn_t) http_rewrite_free);
    } else if (!strcmp(htype, "rfun")) {
	SEXP sFn = CAR(args); args = CDR(args);
	if (!isFunction(sFn) && !isSymbol(sFn))
	    Rf_error("fn must be a function");
	/* RIF_ASYNC just means that a mutex will be used before evaluations.
	   Also once this is enabled, the mutex is used for *all* evaluations
	   even for handlers that did not specify it. */
	int flags = RIF_ASYNC | RIF_PARSE_QUERY | RIF_PARSE_HEADERS;
	SEXP s = findArg(args, "fork", 0);
	/* The fork handler does not use the synchronization mutex,
	   but the child will use the regular handler, so it may use it.
	   Thus we may want to remove RIF_ASYNC, but probably more sensible
	   would be for the child to remove the r_queue - uless we want
	   to also split the I/O in the child from R evaluation .... */
	if (s && Rf_asInteger(s))
	    flags |= RIF_FORK;
	s = findArg(args, "parse.headers", 0);
	if (s && !Rf_asInteger(s))
	    flags ^= RIF_PARSE_HEADERS;
	s = findArg(args, "parse.query", 0);
	if (s && !Rf_asInteger(s))
	    flags ^= RIF_PARSE_QUERY;
	add_content_handler(r_inline_handler,
			    r_inline_entry(CHAR(STRING_ELT(sPrefix, 0)),
					   sFn, flags),
			    NULL, (free_handler_fn_t) r_inline_free);
    } else
	Rf_error("Unknown handler type '%s'", htype);
    return ScalarLogical(1);
}

SEXP C_remove_all_handlers() {
    free_content_handlers();
    return ScalarLogical(1);
}

SEXP C_parse_headers(SEXP sWhat) {
    return r_http_parse_headers((const char*)RAW(sWhat), (int) LENGTH(sWhat));
}

/* parses a URL encoded form (raw vector) into a named character vector */
SEXP C_parse_query(SEXP sWhat) {
    char *buf;
    if (TYPEOF(sWhat) != RAWSXP)
	Rf_error("invalid form body");
    if (memchr(RAW(sWhat), 0, XLENGTH(sWhat)))
	Rf_error("form body contains NUL bytes");
    buf = R_alloc(XLENGTH(sWhat) + 1, 1);
    memcpy(buf, RAW(sWhat), XLENGTH(sWhat));
    buf[XLENGTH(sWhat)] = 0;
    return r_http_parse_query(buf);
}

/* element of a named list or R_NilValue */
static SEXP list_elt(SEXP list, const char *name) {
    SEXP names = Rf_getAttrib(list, R_NamesSymbol);
    int i, n = (TYPEOF(list) == VECSXP) ? LENGTH(list) : 0;
    for (i = 0; i < n && TYPEOF(names) == STRSXP; i++)
	if (!strcmp(CHAR(STRING_ELT(names, i)), name))
	    return VECTOR_ELT(list, i);
    return R_NilValue;
}

/* file name or NULL if not set (NULL/NA); OpenSSL expects UTF-8 on Windows */
static const char *file_elt(SEXP list, const char *name) {
    SEXP s = list_elt(list, name);
    if (TYPEOF(s) != STRSXP || LENGTH(s) < 1 || STRING_ELT(s, 0) == NA_STRING)
	return 0;
#ifdef WIN32
    return Rf_translateCharUTF8(STRING_ELT(s, 0));
#else
    return Rf_translateChar(STRING_ELT(s, 0));
#endif
}

static const char *spec_elt(SEXP l) {
    return CHAR(STRING_ELT(list_elt(l, "spec"), 0));
}

#define MAX_LISTENERS 64

/* listen is a list of listeners as created by webstart(), each a list with
   spec (string for messages), port, local, ipv6, socket (string or NULL)
   and tls (NULL or a list with cert, key, ca, client.ca, client.cn) */
SEXP C_start(SEXP sListen, SEXP sThreads, SEXP sFork) {
    int threads = asInteger(sThreads), use_fork = asInteger(sFork), flags = 0;
    int i, n, failed = -1;
    server_t *srv[MAX_LISTENERS];
    tls_t *tls[MAX_LISTENERS];
    const char *sock[MAX_LISTENERS];
    char err[512];

    if (TYPEOF(sListen) != VECSXP || (n = LENGTH(sListen)) < 1)
	Rf_error("there must be at least one listener");
    if (n > MAX_LISTENERS)
	Rf_error("too many listeners (at most %d are supported)", MAX_LISTENERS);

    /* anything that can raise an R error must be done before resources
       are allocated, servers are bound or threads started */
    if (use_fork && threads > 0) {
	Rf_warning("fork=TRUE and threads > 0 are mutually exclusive, setting threads to 0");
	threads = 0;
    }

    for (i = 0; i < n; i++) {
	SEXP l = VECTOR_ELT(sListen, i), sSock;
	if (TYPEOF(list_elt(l, "spec")) != STRSXP || LENGTH(list_elt(l, "spec")) != 1)
	    Rf_error("invalid listener specification");
	sSock = list_elt(l, "socket");
	sock[i] = (TYPEOF(sSock) == STRSXP && LENGTH(sSock) == 1) ?
	    Rf_translateChar(STRING_ELT(sSock, 0)) : 0;
#ifdef WIN32
	if (sock[i])
	    Rf_error("Local sockets are not supported by Windows");
#endif
#ifndef HAVE_IPV6
	if (asLogical(list_elt(l, "ipv6")) == TRUE)
	    Rf_error("IPv6 support is not enabled in this build, cannot listen on %s", spec_elt(l));
#endif
    }

    if (!list_content_handlers(0, 0))
	Rf_warning("No content handlers are registered, all responses will be 404.");

    /* create TLS contexts first, so errors are reported before anything is started */
    memset(srv, 0, sizeof(srv));
    memset(tls, 0, sizeof(tls));
    for (i = 0; i < n; i++) {
	SEXP t = list_elt(VECTOR_ELT(sListen, i), "tls");
	if (t != R_NilValue) {
	    SEXP sCN = list_elt(t, "client.cn");
	    int j, n_cn = (TYPEOF(sCN) == STRSXP) ? LENGTH(sCN) : 0;
	    const char **cn = (const char**) R_alloc(n_cn + 1, sizeof(const char*));
	    for (j = 0; j < n_cn; j++)
		cn[j] = Rf_translateCharUTF8(STRING_ELT(sCN, j));
	    err[0] = 0;
	    tls[i] = new_tls(file_elt(t, "cert"), file_elt(t, "key"), file_elt(t, "ca"),
			     file_elt(t, "client.ca"), cn, n_cn, err, sizeof(err));
	    if (!tls[i]) {
		for (j = 0; j < i; j++)
		    free_tls(tls[j]);
		Rf_error("TLS setup for %s failed: %s", spec_elt(VECTOR_ELT(sListen, i)), err);
	    }
	}
    }

    begin_pool("Rmain");

    ulog_set_app_name("Rwebserver");

    r_inline_setup(threads > 0 ? RIF_ASYNC : 0);

    if (use_fork)
	flags |= SRV_FORK;
    if (threads > 0)
	flags |= (SRV_ASYNC_PROCESS | SRV_NONBLOCK);
    /* we do not allow SRV_ASYNC_ACCEPT just yet, but we could ... */
    DBG(flags |= SRV_LOG);

    for (i = 0; i < n; i++) {
	SEXP l = VECTOR_ELT(sListen, i), sAddr = list_elt(l, "address");
	int is_tls = (tls[i] != 0); /* tls[i] is cleared once the server owns it */
	const char *proto = is_tls ? "HTTPS" : "HTTP";
	int lflags = flags;
	if (asLogical(list_elt(l, "ipv6")) == TRUE)
	    lflags |= SRV_IPV6;
	if (asLogical(list_elt(l, "local")) == TRUE)
	    lflags |= SRV_LOCAL;
	const char *addr = (TYPEOF(sAddr) == STRSXP && LENGTH(sAddr) == 1) ? CHAR(STRING_ELT(sAddr, 0)) : 0;
	if (!(srv[i] = create_HTTP_server(asInteger(list_elt(l, "port")), addr, sock[i], lflags, http_request, 0))) {
	    failed = i;
	    break;
	}
	if (tls[i]) { /* the server owns the context from now on */
	    http_server_set_tls(srv[i], tls[i]);
	    tls[i] = 0;
	}
	if (!sock[i]) {
	    client_connection_info_t info;
	    if (!server_get_info(srv[i], &info)) {
		int port = (int) ntohs(info.sa.ip4.sin_port);
		ulog_info("started %s server on %s port %d", proto,
			  (info.sa.ip4.sin_family == AF_INET) ? "IPv4" : "IPv6", port);
		/* the port was chosen by the system, so the user needs to know it */
		if (asInteger(list_elt(l, "port")) == 0) {
		    int v6 = (lflags & SRV_IPV6) ? 1 : 0;
		    REprintf("Listening on %s://%s%s%s:%d\n", is_tls ? "https" : "http",
			     v6 ? "[" : "", addr ? addr : ((lflags & SRV_LOCAL) ? "localhost" : "*"),
			     v6 ? "]" : "", port);
		}
	    } else
		ulog_info("started %s server on %s", proto, spec_elt(l));
	} else
	    ulog_info("started HTTP server on socket '%s'", sock[i]);
    }

    if (failed < 0) {
	if (threads > 0)
	    http_start_threads(threads);
	ulog_info("starting server loop");
	serverLoop();
    }
    /* the processing threads may have been started by create_HTTP_server() */
    http_finish_threads();
    r_inline_end();
    /* keep the reason, cleaning up may overwrite it */
    if (failed >= 0) {
	const char *e = server_last_error();
	snprintf(err, sizeof(err), "%s", (e && *e) ? e : "unknown error");
    }
    for (i = 0; i < n; i++) {
	if (srv[i]) {
	    rm_server(srv[i]);
	    free(srv[i]);
	}
	free_tls(tls[i]);
    }
    if (failed < 0)
	ulog_info("clean shutdown");
#ifdef ALLOC_REPORT
    report_pool();
#endif
    end_pool();
#ifdef ALLOC_REPORT
    alloc_global_report();
#endif
    if (failed >= 0)
	Rf_error("failed to start server on %s: %s", spec_elt(VECTOR_ELT(sListen, failed)), err);
    return R_NilValue;
}

SEXP C_has_tls(void) {
#ifdef HAVE_TLS
    return ScalarLogical(1);
#else
    return ScalarLogical(0);
#endif
}

#endif
