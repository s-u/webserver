#ifndef HTTP_H__
#define HTTP_H__

#ifndef NO_CONFIG_H
#include "config.h"
#endif

#include "dalloc.h"

#ifdef NO_WEBSOCKETS
#include "server.h"
#else
#include "websockets.h"
#endif

/* bit-wise processing flags set by handlers that want to pass information
   to subsequent handlers. Those flags are not used by http.c itself,
   but handlers can communicate state to each other this way.
*/
#define PF_REWRITE_STOP  1 /* rewrite rule is reqeusting no more rewrites */
#define PF_AUTHORIZED    2 /* authorization handler succeeded */

/* URL: <scheme>:[//<authority>]<path>[?<query>][#<fragment>]

   NOTE: the request elements may NOT be modified by handlers with
   the exception of using http_request_set_url() which is intended for
   rewrite handlers that can modify the URL parts.

   Also fragment is typically not used by URLs in HTTP requests but kept
   only on the client side. We keep it here for completeness enabling some
   interesting protocol uses, but most handlers will ignore it. */
typedef struct {
    char  *url;      /* actually path */
    char  *body;
    size_t body_len;
    char  *query;
    char  *fragment;
    char  *content_type;
    char  *headers;
    char  *auth;
    double date;
    char   method[8]; /* first 7 characters of the method */
    int    processing_flags;
} http_request_t;

#define PAYLOAD_VERBATIM 0 /* payload points to the bytes to send */
#define PAYLOAD_FILE     1 /* file */
#define PAYLOAD_TEMPFILE 2 /* file which will be deleted once served */

#define RF_USE_RANGE  1  /* if set the result ranges are populated */
#define RF_IS_SUBSET  2  /* if set the the result handler should assume
			    that the payload already reflects the requested
			    range (otherwise it will extract the range) */
#define RF_REFUSED    8  /* this an internal flag is set by handers that matched,
			    but for some reason decided not to process the request
			    and they cannot use the return value to signal that.
			    It is only used for internal communication and
			    shold not be seen outside of internal code. */

/* all members are set to 0 before a handler is called.
   non-NULL pointers will be free()d once the request is
   processed so do not use static buffers. */
typedef struct {
    char  *err;
    char  *payload;
    size_t payload_len;
    size_t range_start, range_end;
    int    payload_type;   /* see PAYLOAD_xx constants above */
    char  *content_type; /* defaults to "text/html" if NULL */
    char  *headers;
    double date; /* 0=now, <0 do not send Date: header */
    int    code; /* 0 => 200 */
    int    flags;
} http_result_t;

typedef void (*http_handler_fn_t)(http_request_t *req, http_result_t *res);

int http_request_set_url(http_request_t *req, const char *path, const char *query, const char *fragment);

int http_start_threads(int n);
int http_finish_threads();

/* flags */
#define HTTP_WS_UPGRADE 0x10 /* allow WebSockets UPGRADE */
/* Note: the following was used in Rserve, but has no effect here */
#define HTTP_RAW_BODY   0x20 /* if set, no attempts are made to decode the request body of known types */

#ifdef NO_WEBSOCKETS
server_t *create_HTTP_server(int port, int flags, http_handler_fn_t handler, void *dummy);
#else
server_t *create_HTTP_server(int port, int flags, http_handler_fn_t handler, ws_connected_fn_t ws_connected);
#endif

#endif
