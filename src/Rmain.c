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
	if (sIndex != R_NilValue && (!isString(sPath) || LENGTH(sPath) != 1))
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

SEXP C_start(SEXP sPort, SEXP sSock, SEXP sThreads, SEXP sFork, SEXP sIPv6, SEXP sLocal) {
    int http_port = asInteger(sPort), threads = asInteger(sThreads); /* tls_info = 0; */
    int use_fork = asInteger(sFork), use_ip6 = asInteger(sIPv6) > 0, flags = 0;
    int use_local = asInteger(sLocal);
    int active_servers = 0;
    const char *sock = 0;
    const char *doc_root = "./";

    if (!list_content_handlers(0, 0))
	Rf_warning("No content handlers are registered, all responses will be 404.");

    if (TYPEOF(sSock) == STRSXP && LENGTH(sSock) == 1)
	sock = CHAR(STRING_ELT(sSock, 0));
    else if (sSock != R_NilValue)
	Rf_error("Invalid value for `socket', must be a string or NULL");

#ifdef WIN32
    if (sock)
	Rf_error("Local sockets are not supported by Windows");
#endif

    begin_pool("Rmain");

    ulog_set_app_name("Rwebserver");

    if (use_fork && threads > 0) {
	Rf_warning("fork=TRUE and threads > 0 are mutually exclusive, setting threads to 0");
	threads = 0;
    }

    r_inline_setup(threads > 0 ? RIF_ASYNC : 0);

    if (use_fork)
	flags |= SRV_FORK;
    if (threads > 0)
	flags |= (SRV_ASYNC_PROCESS | SRV_NONBLOCK);
    if (use_ip6)
	flags |= SRV_IPV6;
    if (use_local)
	flags |= SRV_LOCAL;
    /* we do not allow SRV_ASYNC_ACCEPT just yet, but we could ... */
    DBG(flags |= SRV_LOG);

    server_t *http_server = 0;
    http_server = create_HTTP_server(http_port, sock, flags, http_request, 0);
    if (http_server) {
	if (!sock) {
	    client_connection_info_t info;
	    if (!server_get_info(http_server, &info))
		ulog_info("started HTTP server on %s port %d",
			      (info.sa.ip4.sin_family == AF_INET) ? "IPv4" : "IPv6",
			  (int) ntohs(info.sa.ip4.sin_port));
	    else
		ulog_info("started HTTP server on port %d", http_port);
	} else
	    ulog_info("started HTTP server on socket '%s'", sock);
	active_servers++;
	if (threads > 0)
	    http_start_threads(threads);
    } else
	Rf_error("failed to start HTTP server on port %d", http_port);

    if (!active_servers)
	Rf_error("there are no active servers, aborting.");
    ulog_info("starting server loop (http=%d, doc_root='%s')", http_port, doc_root);
    serverLoop();
    if (threads > 0)
	http_finish_threads();
    r_inline_end();
    rm_server(http_server);
    free(http_server);
    ulog_info("clean shutdown");
#ifdef ALLOC_REPORT
    report_pool();
#endif
    end_pool();
#ifdef ALLOC_REPORT
    alloc_global_report();
#endif
    return R_NilValue;
}

#endif
