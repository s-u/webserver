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
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#ifndef WIN32
#include <signal.h>
#include <sys/wait.h>
#include <sys/time.h>
#endif

#include <Rinternals.h>

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
	    char     method[8];
	    uint32_t res2, res3, res4, res5, res6;
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

/* IMPORTANT: the resulting request header just uses pointers into the msg,
   so it may NOT be freed by anyone else */
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
    memcpy(msg->h.req.method, r->method, sizeof(msg->h.req.method));
    return (size_t) len;
}

static void parse_request_hdr(http_request_t *r, struct rmsg *msg, char *buf) {
    DBG(ulog_debug("r_ext: parse_request_hdr: path=%d, query=%d, fragment=%d, ctype=%d, headers=%d [%d/%d]",
		   msg->h.req.path, msg->h.req.query, msg->h.req.fragment,
		   msg->h.req.content_type, msg->h.req.headers,
		   (int) msg->hdr_len, (int) msg->body_len));
#ifdef CLONE_REQ
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
#else
    r->url = (msg->h.req.path >= 0) ? (buf + msg->h.req.path) : 0;
    r->query = (msg->h.req.query >= 0) ? (buf + msg->h.req.query) : 0;
    r->fragment = (msg->h.req.fragment >= 0) ? (buf + msg->h.req.fragment) : 0;
    r->content_type = (msg->h.req.content_type >= 0) ? (buf + msg->h.req.content_type) : 0;
    r->headers = (msg->h.req.headers >= 0) ? (buf + msg->h.req.headers) : 0;
    r->auth = (msg->h.req.auth >= 0) ? (buf + msg->h.req.auth) : 0;
    r->date = msg->date;
    r->processing_flags = msg->h.req.processing_flags;
#endif
    memcpy(r->method, msg->h.req.method, sizeof(r->method));
    r->method[sizeof(r->method) - 1] = 0;
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

static void parse_response_hdr(http_result_t *r, struct rmsg *msg, char *buf, int clone) {
    if (clone) { /* the result must be legal, i.e. everything copied */
	if (r->headers)
	    free(r->headers);
	r->headers = (msg->h.res.headers >= 0) ? strdup(buf + msg->h.res.headers) : 0;
	if (r->content_type)
	    free(r->content_type);
	r->content_type = (msg->h.res.content_type >= 0) ? strdup(buf + msg->h.res.content_type) : 0;
	if (r->err)
	    free(r->err);
	r->err = (msg->h.res.err >= 0) ? strdup(buf + msg->h.res.err) : 0;
	if (r->payload)
	    free(r->payload);
	r->payload = 0;
	if (msg->body_len) {
	    /* the extra byte guarantees termination, since file payloads
	       carry a file name which is used as a string */
	    r->payload = malloc(msg->body_len + 1);
	    if (r->payload) {
		memcpy(r->payload, buf + msg->hdr_len, msg->body_len);
		r->payload[msg->body_len] = 0;
	    } else {
		ulog_error("r_ext.parse_response_hdr: cannot allocate memory for body (%lu bytes)",
			   (unsigned long) msg->body_len);
		r->code = 500;
		return;
	    }
	}
    } else {
	r->headers = (msg->h.res.headers >= 0) ? (buf + msg->h.res.headers) : 0;
	r->content_type = (msg->h.res.content_type >= 0) ? (buf + msg->h.res.content_type) : 0;
	r->err = (msg->h.res.err >= 0) ? (buf + msg->h.res.err) : 0;
	r->payload = msg->body_len ? (buf + msg->hdr_len) : 0;
    }
    r->code = msg->h.res.code;
    r->flags = msg->h.res.flags;
    r->payload_type = msg->h.res.payload_type;
    r->payload_len = msg->body_len;
    r->range_start = msg->h.res.range_start;
    r->range_end = msg->h.res.range_end;
    r->date = msg->date;
}

/* state */
#define RCS_HDR    0
#define RCS_MSG    1
#define RCS_RECVD  2
#define RCS_ABORT -1
#define RCS_DEAD  -2

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

#ifndef WIN32
/* finalizer of the connections to forked workers */
static void r_conn_fin(client_connection_t *c) {
    DBG(ulog_debug("r_ext: finalizing worker %p (pid=%d)", (void*) c, (int) (c ? c->pid : -1)));
    if (!c)
	return;
    if (c->info.s != INVALID_SOCKET) {
	closesocket(c->info.s);
	c->info.s = -1;
    }
    if (c->pid) { /* are we the parent of a worker ? */
	int stat = 0;
	pid_t wp = 0;
	/* FIXME: it would be nice to wait a bit, since the worker may take some time
	   to register that we are shutting down. In order to do that, we should
	   create a pool where we send all workers to finish so we don't have to
	   wait for them one by one */
	/* NOTE: the server loop reaps any terminated children, so waitpid() can
	   fail with ECHILD. Only a result of 0 means the worker is still running
	   and the pid is safe to signal (otherwise it may have been re-used). */
	if ((wp = waitpid(c->pid, &stat, WNOHANG)) == 0) {
	    DBG(ulog_debug("r_ext: r_conn_fin worker %p (pid=%d) has not terminated, sending SIGHUP",
			   (void*) c, (int) c->pid));
	    /* that should be enough to break any waiting loops */
	    kill(c->pid, SIGHUP);
	    DBG(ulog_debug("r_ext: r_conn_fin - waiting for worker %p (pid=%d)",
			   (void*) c, (int) c->pid));
	    /* wait up to 100ms. NOTE: we must not use SIGALRM/sigwait for this,
	       because the signal is process-wide and its default action would
	       terminate R if any other thread had it unblocked. */
	    int tries = 0;
	    while ((wp = waitpid(c->pid, &stat, WNOHANG)) == 0 && tries++ < 100) {
		struct timespec ts = { 0, 1000000 }; /* 1ms */
		nanosleep(&ts, NULL);
	    }
	    if (wp == 0) {
		/* FIXME: in case there is a problem we may not want to wait indefinitely
		   see above for a possibly better apporach ...*/
		kill(c->pid, SIGKILL);
		wp = waitpid(c->pid, &stat, 0);
	    }
	}
	DBG(ulog_debug("r_ext: r_conn_fin - wait result for worker %p (pid=%d) = %d (stat=%d)",
		       (void*) c, (int) c->pid, (int) wp, stat));
    }
    if (c->buf)
	free(c->buf);
}
#endif

static void r_msg_finished(client_connection_t *c) {
    /* properly processed message */
    if (c->state == RCS_RECVD) {
	c->pos = 0;
	c->hdr_pos = 0;
	c->state = RCS_HDR;
	return;
    }
    /* otherwise something went wrong */
    if (c->info.s != INVALID_SOCKET) {
	closesocket(c->info.s);
	c->info.s = INVALID_SOCKET;
	c->state = RCS_ABORT;
    }
}

static void r_process(client_connection_t *c) {
    server_t *srv = c->info.srv;
    DBG(ulog_debug("r_ext: r_process %p (%d), state=%d, hdr=%d/%d, body=%d/%d[/%d]",
		   (void*) c, (int) c->info.s, c->state,
		   (int) c->hdr_pos, (int) sizeof(struct rmsg),
		   (int) c->pos, (int) c->len, (int) c->alloc));
    if (c->state == RCS_HDR) {
	if (c->hdr_pos < sizeof(struct rmsg)) {
	    int n = srv->recv(c, ((char*)(&c->hdr)) + c->hdr_pos, sizeof(struct rmsg) - c->hdr_pos);
	    if (n < 1) {
		/* shutdown request will close the connection and set
		   the FD to -1 in which case we don't need to do the close */
		if (c->info.s != INVALID_SOCKET) {
		    if (n)
			ulog_error("r_ext: error reading from R worker (%s), closing connection",
				   strerror(errno));
		    else
			DBG(ulog_debug("r_ext: remote R worker closed connection"));
		    closesocket(c->info.s);
		    c->info.s = INVALID_SOCKET;
		} else
		    DBG(ulog_debug("r_ext: connection closed due to signal"));
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
#if 0 /* dump the payload */
	    DBG(if (c->len) {
		    size_t i = 0;
		    while (i < c->len) {
			if (!(i & 15))
			    fprintf(stderr, "\n%04x ", (int) i);
			fprintf(stderr, " %02x", (int)(c->buf[i++]));
		    }
		    fprintf(stderr, "\n");
		});
#endif
	    http_request_t req;
	    http_result_t  res;
	    memset(&req, 0, sizeof(req));
	    memset(&res, 0, sizeof(res));
	    parse_request_hdr(&req, &c->hdr, c->buf);
	    req.body = c->hdr.body_len ? body : 0;
	    req.body_len = (size_t) c->hdr.body_len;
	    /* IMPORTANT: we are relying r_inline_handler NOT changing req,
	       becasue we use our special references-to-one-big-buffer approach,
	       so those may not be freed by the handler */
	    int n = r_inline_handler(&req, &res, c->ri);
	    DBG(ulog_debug("r_inline_handler returned %d", n));
	    /* r_inline_handler clears RF_REFUSED and signals it via the return
	       value, but the parent can only see the flag, so put it back */
	    if (!n)
		res.flags |= RF_REFUSED;
	    /* file payloads are sent as the (NUL-terminated) file name */
	    if (res.payload && (res.payload_type == PAYLOAD_FILE || res.payload_type == PAYLOAD_TEMPFILE))
		res.payload_len = strlen(res.payload);
	    size_t l = build_response_hdr(&res, &c->hdr, c->buf, c->alloc);
	    if (l > c->alloc) {
		free(c->buf);
		c->alloc = l + 16;
		c->buf = (char*) malloc(c->alloc);
		if (!c->buf) {
		    /* we freed buf which is all we used for allocation so we're ok */
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
	    if (send_all(c, &c->hdr, sizeof(c->hdr), 0, 0) <= 0 ||
		(l && send_all(c, c->buf, l, 0, 0) <= 0) ||
		(res.payload_len && send_all(c, res.payload, res.payload_len, 0, 0) <= 0)) {
		DBG(ulog_debug("r_ext: failed to send response, closing connection"));
		closesocket(c->info.s);
		c->info.s = INVALID_SOCKET;
	    }
	    /* result is allocated by the regular R handler, so that needs freeing */
	    http_free_result(&res);
	} else if (c->hdr.cmd == CMD_HTTP_RESP) {
	    /* we do not handle that here, go out */
	    return;
	}
	/* successfully responded -> back to HDR */
	r_msg_finished(c);
    }
}

#ifndef WIN32
/* the virtual server of the connections to forked workers */
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
#endif

int r_ext_one_request(client_connection_t *c, http_request_t *req, http_result_t *res) {
    size_t l = build_request_hdr(req, &c->hdr, c->buf, c->alloc);
    if (l > c->alloc) {
	if (c->buf)
	    free(c->buf);
	c->alloc = l + 16;
	if (c->alloc < MIN_BUF_SIZE)
	    c->alloc = MIN_BUF_SIZE;
	c->buf = (char*) malloc(c->alloc);
	if (!c->buf) {
	    c->alloc = 0;
	    return -1;
	}
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
    ssize_t n = send_all(c, &c->hdr, sizeof(c->hdr), 0, 0);
    DBG(ulog_debug("r_ext.r_one_request: send header = %d", (int) n));
    if (n <= 0) {
	ulog_warn("r_ext.r_one_request: failed to send request to worker %p (pid=%d)",
		  (void*) c, (int) c->pid);
	if (c->info.s != INVALID_SOCKET){
	    closesocket(c->info.s);
	    c->info.s = INVALID_SOCKET;
	}
	return -2;
    }
    if (l) {
	n = send_all(c, c->buf, l, 0, 0);
	DBG(ulog_debug("r_ext.r_one_request: send hdr_len = %d", (int) n));
	if (n <= 0) {
	    if (c->info.s != INVALID_SOCKET){
		closesocket(c->info.s);
		c->info.s = INVALID_SOCKET;
	    }
	    return -2;
	}
    }
    if (req->body_len) {
	n = send_all(c, req->body, req->body_len, 0, 0);
	DBG(ulog_debug("r_ext.r_one_request: send body = %d", (int) n));
	if (n <= 0) {
	    if (c->info.s != INVALID_SOCKET){
		closesocket(c->info.s);
		c->info.s = INVALID_SOCKET;
	    }
	    return -2;
	}
    }
    DBG(ulog_debug("r_ext.r_one_request: waiting for response"));
    while (c->info.s != INVALID_SOCKET && c->state != RCS_RECVD)
	r_process(c);
    DBG(ulog_debug("r_ext.r_one_request: returned with state %d", c->state));
    if (c->state != RCS_RECVD)
	/* something went wrong and the connection is closed */
	return -1;
    parse_response_hdr(res, &c->hdr, c->buf, 1);
    r_msg_finished(c);
    return 0;
}

int r_ext_ready(client_connection_t *c) {
    return (c && c->info.s != INVALID_SOCKET && c->state == RCS_HDR) ? 1 : 0;
}

void r_ext_close(client_connection_t *c) {
    if (!c)
	return;
    if (c->info.s != INVALID_SOCKET) {
	DBG(ulog_debug("r_ext_close: requested for active connection %p (%d)",
		       (void*) c, (int) c->info.s));
	closesocket(c->info.s);
	c->info.s = INVALID_SOCKET;
	c->state = RCS_ABORT;
#ifndef WIN32
	if (c->pid)
	    kill(c->pid, SIGHUP);
#endif
    }
}

#ifndef WIN32
/* SIGHUP is how the parent tells the worker to go away. Closing the
   connection is not enough, because workers forked later inherit the
   parent's end of it, so the worker may never see EOF. */
static void sighandler(int sig) {
    _exit(0);
}
#endif

client_connection_t *r_ext_new_worker(r_inline *ri) {
#ifdef WIN32
    ulog_error("Windows does not support forked workers.");
    return 0;
#else
    pid_t f;
    client_connection_t *c = (client_connection_t*) calloc(sizeof(client_connection_t), 1);
    int ios[2];
    if (!c)
	return c;
    if (!remote_server) /* virtual server */
	remote_server = new_remote_server(0);

    if (socketpair(AF_LOCAL, SOCK_STREAM, 0, ios)) {
	ulog_error("r_new_worker: socketpair() failed with %s", strerror(errno));
	free(c);
	return 0;
    }
    /* flush stdio so the child does not inherit (and later re-emit)
       buffered output of the parent */
    fflush(NULL);
    f = fork();
    if (f < 0) {
	ulog_error("r_new_worker: fork() failed with %s when creating a new worker process",
		   strerror(errno));
	closesocket(ios[0]);
	closesocket(ios[1]);
	free(c);
	return 0;
    }
    if (!f) { /* child */
	DBG(ulog_debug("r_new_worker: child process %d started", (int) getpid()));
	closesocket(ios[0]);
	/* the worker must not keep the listening socket or any client connection
	   of the parent alive (the client would never see the connection close) */
	close_server_sockets_in_child(ios[1]);
	r_inline_fork_child();
	c->info.s = ios[1];
	c->info.srv = remote_server;
	c->ri = r_inline_worker_entry(ri);
#ifndef WIN32
	{
	    sigset_t set;
	    signal(SIGHUP, sighandler);
	    sigemptyset(&set);
	    sigaddset(&set, SIGHUP);
	    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
	}
#endif
	while (c->info.s != INVALID_SOCKET && c->state != RCS_ABORT) {
	    r_process(c);
	}
	if (c->info.srv->conn_fin)
	    c->info.srv->conn_fin(c);
	free(c);
	free(remote_server);
	DBG(ulog_debug(" - child process %d terminating", (int) getpid()));
	/* NOT exit(): that would run atexit handlers and stdio clean-up of the
	   parent's state. In particular, it re-positions inherited input streams,
	   which share the file offset with the parent, so e.g. Rscript would
	   continue reading its script from the wrong place.
	   Output buffered by the child itself is flushed explicitly (nothing
	   is inherited since we flush before fork()). */
	fflush(NULL);
	_exit(0);
    }
    /* parent */
    closesocket(ios[1]);
    c->info.s = ios[0];
    c->pid = f;
    c->info.srv = remote_server;
    c->ri = ri;
    return c;
#endif /* !WIN32 */
}
