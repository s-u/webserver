#ifndef HTTP_H__
#define HTTP_H__

#ifdef NO_WEBSOCKETS
#include "server.h"
#else
#include "websockets.h"
#endif

/* URL: <scheme>:[//<authority>]<path>[?<query>][#<fragment>] */
typedef struct {
    char  *url;      /* path */
    char  *body;
    size_t body_len;
    char  *query;
    char  *fragment;
    char  *headers;
    double date;
    char  method[4]; /* first 3 characters of the method + NUL (e.g. "HEA" for HEAD)*/
} http_request_t;

#define PAYLOAD_VERBATIM 0 /* payload points to the bytes to send */
#define PAYLOAD_FILE     1 /* file */
#define PAYLOAD_TEMPFILE 2 /* file which will be deleted once served */

/* all members are set to 0 before a handler is called.
   non-NULL pointers will be free()d once the request is
   processed so do not use static buffers. */
typedef struct {
    char  *err;
    char  *payload;
    size_t payload_len;
    int    payload_type;   /* see PAYLOAD_xx constants above */
    char  *content_type; /* defaults to "text/html" if NULL */
    char  *headers;
    double date; /* 0=now, <0 do not send Date: header */
    int code; /* 0 => 200 */
} http_result_t;

typedef void (*http_handler_fn_t)(http_request_t *req, http_result_t *res);

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
