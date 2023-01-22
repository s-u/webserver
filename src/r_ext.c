/* R content HTTP handler.

   (C)Copyright 2022 by Simon Urbanek <simon.urbanek@R-project.org>
   License: MIT

*/

#include "ulog.h"
#include "http.h"
#include "http_tools.h"
#include "http_date.h"
#include "async.h"
#include "r_inline.h"
#include "r_http_tools.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

#include <Rinternals.h>

static async_queue *r_queue;

void r_ext_setup(int flags) {
    if (!r_queue) {
	r_queue = async_queue_new();
    }
}

void r_ext_end() {
    if (r_queue) {
	async_queue_finish(r_queue);
	async_queue_free(r_queue);
	r_queue = 0;
    }
}

#define CMD_HTTP_REQ  1
#define CMD_HTTP_RESP 2

struct rmsg {
    uint32_t cmd;
    uint32_t msg_flags;
    uint64_t hdr_len;
    uint64_t body_len;
    uint64_t res;
    double   date;
    union {
	struct rmsg_req {
	    int32_t  path, query, fragment, content_type, headers, auth;
	    int32_t  processing_flags;
	    uint32_t res0, res1, res2, res3, res4, res5, res6;
	} req;
	struct rmsg_res {
	    int32_t  code, headers, err, content_type, flags, payload_type;
	    uint64_t range_start, range_end;
	    uint32_t res3, res4, res5, res6;
	} res;
    } h;
};

static int check_req_hdr(struct rmsg_req *h, size_t mvo) {
    return ((h->path >= 0 && h->path >= mvo) ||
	    (h->query >= 0 && (h->query >= mvo || h->query < h->path)) ||
	    (h->fragment >= 0 && (h->fragment >= mvo || h->fragment < h->path)) ||
	    (h->content_type >= 0 && h->content_type >= mvo) ||
	    (h->headers >= 0 && h->headers >= mvo) ||
	    (h->auth >= 0 && h->auth >= mvo)) ? 0 : 1;
}

static size_t build_request_hdr(http_request_t *r, struct rmsg *msg, char *buf, size_t buf_len) {
    int32_t len = 0, il;
#define MSG_INSERT(R, H) if (R) {\
	H = len;\
	len += (il = strlen(R) + 1);\
	if (len <= buf_len) {\
	    memcpy(buf, R, il);\
	    buf += il;\
	}\
    } else {\
	H = -1;\
    }
    MSG_INSERT(r->url, msg->h.req.path);
    MSG_INSERT(r->query, msg->h.req.query);
    MSG_INSERT(r->fragment, msg->h.req.fragment);
    MSG_INSERT(r->content_type, msg->h.req.content_type);
    MSG_INSERT(r->headers, msg->h.req.headers);
    MSG_INSERT(r->auth, msg->h.req.auth);
    msg->date = r->date;
    msg->h.req.processing_flags = r->processing_flags;
    return (size_t) len;
}

static void parse_request_hdr(http_request_t *r, struct rmsg *msg, const char *buf) {
    http_request_set_url(r,
			 (msg->h.req.path >= 0) ? (buf + msg->h.req.path) : 0,
			 (msg->h.req.query >= 0) ? (buf + msg->h.req.query) : 0,
			 (msg->h.req.fragment >= 0) ? (buf + msg->h.req.fragment) : 0);
    if (r->content_type)
	free(r->content_type);
    r->content_type = (msg->h.req.content_type >= 0) ? strdup(buf + msg->h.req.content_type) : 0;
    if (r->headers)
	free(r->headers);
    r->headers = (msg->h.req.headers >= 0) ? strdup(buf + msg->h.req.headers) : 0;
    if (r->auth)
	free(r->auth);
    r->auth = (msg->h.req.auth >= 0) ? strdup(buf + msg->h.req.auth) : 0;
    r->date = msg->date;
    r->processing_flags = msg->h.req.processing_flags;
}

static size_t build_response_hdr(http_result_t *r, struct rmsg *msg, char *buf, size_t buf_len) {
    int32_t len = 0, il;
    MSG_INSERT(r->err, msg->h.res.err);
    MSG_INSERT(r->content_type, msg->h.res.content_type);
    MSG_INSERT(r->headers, msg->h.res.headers);
    msg->date = r->date;
    msg->h.res.code = r->code;
    msg->h.res.flags = r->flags;
    msg->h.res.range_start = r->range_start;
    msg->h.res.range_end = r->range_end;
    msg->h.res.payload_type = r->payload_type;
    return (size_t) len;
}    

static void parse_response_hdr(http_result_t *r, struct rmsg *msg, char *buf) {
    /* not sure - we could perhaps avoid the copies ... */
#define CL(X) strdup(X)
    if (r->headers)
	free(r->headers);
    r->headers = (msg->h.res.headers >= 0) ? CL(buf + msg->h.res.headers) : 0;
    if (r->content_type)
	free(r->content_type);
    r->content_type = (msg->h.res.content_type >= 0) ? CL(buf + msg->h.res.content_type) : 0;
    if (r->err)
	free(r->err);
    r->err = (msg->h.res.err >= 0) ? CL(buf + msg->h.res.err) : 0;
    r->code = msg->h.res.code;
    r->flags = msg->h.res.flags;
    r->payload_type = msg->h.res.payload_type;
    r->range_start = msg->h.res.range_start;
    r->range_end = msg->h.res.range_end;
    r->date = msg->date;
}

/* state */
#define RCS_HDR   0
#define RCS_MSG   1
#define RCS_RECVD 2

struct client_connection {
    /* common information for all connections */
    client_connection_info_t info;
    /* out stuff */
    pid_t  pid;
    int    state;
    struct rmsg hdr;
    size_t hdr_pos, pos, len, alloc;
    r_inline *ri;
    char  *buf;
};

static void r_conn_fin(client_connection_t *c) {
    DBG(ulog_debug("finalizing worker %p", (void*) c));
    if (!c)
	return;
    if (c->info.s != INVALID_SOCKET) {
	closesocket(c->info.s);
	c->info.s = -1;
    }
    if (c->buf)
	free(c->buf);
}

static void r_process(client_connection_t *c) {
    server_t *srv = c->info.srv;
    if (c->state == RCS_HDR) {
	if (c->hdr_pos < sizeof(struct rmsg)) {
	    int n = srv->recv(c, ((char*)(&c->hdr)) + c->hdr_pos, sizeof(struct rmsg) - c->hdr_pos);
	    if (n < 1) {
		if (n)
		    ulog_error("r_ext: error reading from R worker, closing connection");
		else
		    DBG(ulog_debug("r_ext: remote R worker closed connection"));
		closesocket(c->info.s);
		c->info.s = INVALID_SOCKET;
		return;
	    }
	    c->hdr_pos += n;
	    if (c->hdr_pos < sizeof(struct rmsg))
		return;
	}

	c->state = RCS_MSG;
	/* we got a full header */
	if (!((c->hdr.cmd == CMD_HTTP_REQ && check_req_hdr(&c->hdr.h.req, c->hdr.hdr_len)) ||
	      (c->hdr.cmd == CMD_HTTP_RESP))) {
	    /* unsupported command/invalid header, close (probably corrupted connection) */
	    ulog_error("r_ext: unsupported command (0x%x), aborting", (int) c->hdr.cmd);
	    closesocket(c->info.s);
	    c->info.s = INVALID_SOCKET;
	    return;
	}
	c->len = c->hdr.hdr_len + c->hdr.body_len;
	if (c->len > c->alloc) { /* need bigger buffer? */
	    if (c->buf)
		free(c->buf);
	    c->alloc = c->hdr.hdr_len + c->hdr.body_len;
#define MIN_BUF_SIZE 65536
	    if (c->alloc < MIN_BUF_SIZE)
		c->alloc = MIN_BUF_SIZE;
	    c->buf = (char*) malloc(c->alloc);
	    if (!c->buf) {
		ulog_error("r_ext: out of memory allocating %lu bytes for I/O buffer",
			   (unsigned long) c->alloc);
		closesocket(c->info.s);
		c->info.s = INVALID_SOCKET;
		return;
	    }
	} /* else the content will fit into the existing buffer */
	c->pos = 0;
	/* we leave the body for the next iteration since we cannot
	   guarantee non-blocking read */
	if (c->len)
	    return;
    }
    if (c->state == RCS_MSG) {
	if (c->pos < c->len) {
#define MAX_READ (16*1024*1024)
	    int n = srv->recv(c, c->buf + c->pos,
			      (c->len - c->pos > MAX_READ) ? MAX_READ : (c->len - c->pos));
	    if (n < 1) {
		if (n)
		    ulog_error("r_ext: error reading from R worker, closing connection");
		else
		    DBG(ulog_debug("r_ext: remote R worker closed connection unexpectedly"));
		closesocket(c->info.s);
		c->info.s = INVALID_SOCKET;
		return;
	    }
	    c->pos += n;
	    if (c->pos < c->len)
		return;
	}
	/* completed ! */
	c->state = RCS_RECVD;
	if (c->hdr.cmd == CMD_HTTP_REQ) {
	    char *body = c->buf + c->hdr.hdr_len;
	    DBG(ulog_debug("CMD_HTTP_REQ received"));
	    http_request_t req;
	    http_result_t  res;
	    memset(&req, 0, sizeof(req));
	    memset(&res, 0, sizeof(res));
	    parse_request_hdr(&req, &c->hdr, c->buf);
	    req.body = c->hdr.body_len ? body : 0;
	    req.body_len = (size_t) c->hdr.body_len;
	    int n = r_inline_handler(&req, &res, c->ri);
	    if (n) {};
	    DBG(ulog_debug("r_inline_handler returned %d", n));
	    size_t l = build_response_hdr(&res, &c->hdr, c->buf, c->alloc);
	    if (l > c->alloc) {
		free(c->buf);
		c->alloc = l + 16;
		c->buf = (char*) malloc(c->alloc);
		if (!c->buf) {
		    /* FIXME: free req? */
		    c->alloc = 0;
		    closesocket(c->info.s);
		    c->info.s = INVALID_SOCKET;
		    return;
		}
		l = build_response_hdr(&res, &c->hdr, c->buf, c->alloc);
	    }
	    if (!res.payload)
		res.payload_len = 0;
	    DBG(ulog_debug("r_ext: sending CMD_HTTP_RESP, %ld bytes of headers, %ld body",
			   (long) l, (long) res.payload_len));
	    c->hdr.cmd = CMD_HTTP_RESP;
	    c->hdr.msg_flags = 0;
	    c->hdr.hdr_len = l;
	    c->hdr.body_len = res.payload_len;
	    c->hdr.res = 0;
	    srv->send(c, &c->hdr, sizeof(c->hdr));
	    if (l)
		srv->send(c, c->buf, l);
	    if (res.payload_len)
		srv->send(c, res.payload, res.payload_len);
	    /* successfully responded -> back to HDR */
	    c->state = RCS_HDR;
	} else if (c->hdr.cmd == CMD_HTTP_RESP) {
	    /* we do not handle that here, go out */
	    return;
	}
    }
}

static server_t *remote_server;

static server_t *new_remote_server(int flags) {
    server_t *srv = calloc(sizeof(server_t), 1);
    if (!srv)
	return srv;
    DBG(ulog_debug("new_remote_server(flags=0x%x)", flags));
    srv->recv      = server_recv;
    srv->send      = server_send;
    srv->process   = r_process;
    srv->conn_fin  = r_conn_fin;
    srv->flags     = flags;
    return srv;
}

int r_ext_one_request(client_connection_t *c, http_request_t *req, http_result_t *res) {
    server_t *srv = c->info.srv;
    size_t l = build_request_hdr(req, &c->hdr, c->buf, c->alloc);
    if (l > c->alloc) {
	if (c->buf)
	    free(c->buf);
	c->alloc = l + 16;
	if (c->alloc < MIN_BUF_SIZE)
	    c->alloc = MIN_BUF_SIZE;
	c->buf = (char*) malloc(c->alloc);
	if (!c->buf)
	    return -1;
	l = build_request_hdr(req, &c->hdr, c->buf, c->alloc);
    }
    if (!req->body)
	req->body_len = 0;
    c->hdr.cmd = CMD_HTTP_REQ;
    c->hdr.msg_flags = 0;
    c->hdr.hdr_len = l;
    c->hdr.body_len = req->body_len;
    c->hdr.res = 0;
    DBG(ulog_debug("r_ext.r_one_request: sending CMD_HTTP_REQ, hdr_len=%ld, body_len=%ld",
	     (long) l, (long) req->body_len));
    srv->send(c, &c->hdr, sizeof(c->hdr));
    if (l)
	srv->send(c, c->buf, l);
    if (req->body_len)
	srv->send(c, req->body, req->body_len);
    DBG(ulog_debug("r_ext.r_one_request: waiting for response"));
    while (c->info.s != INVALID_SOCKET && c->state != RCS_RECVD)
	r_process(c);
    DBG(ulog_debug("r_ext.r_one_request: returned with state %d", c->state));
    if (c->state != RCS_RECVD)
	/* something went wrong and the connection is closed */
	return -1;
    parse_response_hdr(res, &c->hdr, c->buf);
    return 0;
}

client_connection_t *r_ext_new_worker(r_inline *ri) {
    pid_t f;
    client_connection_t *c = (client_connection_t*) calloc(sizeof(client_connection_t), 1);
    int ios[2];
    if (!c)
	return c;
    if (!remote_server) /* virtual server */
	remote_server = new_remote_server(0);
    if (socketpair(AF_LOCAL, SOCK_STREAM, 0, ios)) {
	ulog_error("r_new_worker: socketpair() failed with %s", strerror(errno));
	return 0;
    }
    f = fork();
    if (f < 0) {
	ulog_error("r_new_worker: fork() failed with %s when creating a new worker process",
		   strerror(errno));
	return 0;
    }
    if (!f) { /* child */
	DBG(ulog_debug("r_new_worker: child process %d started", (int) getpid()));
	c->info.s = ios[1];
	c->info.srv = remote_server;
	while (c->info.s != INVALID_SOCKET) {
	    r_process(c);
	    /* RCVD state is only after CMD_HTTP_RESP which we will
	       ignore as there is no reply expected */
	    if (c->state == RCS_RECVD)
		c->state = RCS_HDR;
	}
	DBG(ulog_debug(" - child process %d terminating", (int) getpid()));
	exit(0);
    }
    /* parent */
    c->info.s = ios[0];
    c->pid = f;
    c->info.srv = remote_server;
    c->ri = ri;
    return c;
}
