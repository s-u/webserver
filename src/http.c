#include "tls.h"
#include "http.h"
#include "ulog.h"
#include "http_date.h"

#ifndef NO_WEBSOCKETS
#include "websockets.h" /* for connection upgrade */
#endif

#include <string.h>
#include <stdio.h>

/* size of the line buffer for each worker (request and header only)
 * requests that have longer headers will be rejected with 413
 * Note that cookies can be quite big and some browsers send them
 * in one line, so this should not be too small */
#define LINE_BUF_SIZE 32768

/* --- httpd --- */

#define PART_REQUEST 0
#define PART_HEADER  1
#define PART_BODY    2

#define METHOD_POST  1
#define METHOD_GET   2
#define METHOD_HEAD  3
#define METHOD_OTHER 8 /* for custom requests only */

/* attributes of a connection/worker */
#define CONNECTION_CLOSE  0x0001 /* Connection: close response behavior is requested */
#define HOST_HEADER       0x0002 /* headers contained Host: header (required for HTTP/1.1) */
#define HTTP_1_0          0x0004 /* the client requested HTTP/1.0 */
#define CONTENT_LENGTH    0x0008 /* Content-Length: was specified in the headers */
#define THREAD_OWNED      0x0010 /* the worker is owned by a thread and cannot removed */
#define THREAD_DISPOSE    0x0020 /* the thread should dispose of the worker */
#define CONTENT_TYPE      0x0040 /* message has a specific content type set */
#define CONTENT_FORM_UENC 0x0080 /* message content type is application/x-www-form-urlencoded */
#define WS_UPGRADE        0x0100 /* upgrade to WebSockets protocol */
#define BAD_LENGTH        0x0200 /* conflicting or malformed Content-Length: */
#define TRANSFER_ENCODING 0x0400 /* Transfer-Encoding: was specified (unsupported) */

/* used by http_send_respose to limit the size sent at once. Must be at most 31-bit */
#define MAX_SEND_LEN (1024*1024)

/* descriptions to send with 40x status codes, all others will get "Code xxx" instead */
const char *http_40x[] = { "Bad Request", "Unauthorized", "Payment Required", "Forbidden",
						   "Not Found", "Method Not Allowed", "Not Acceptable" };

struct buffer {
    struct buffer *next, *prev;
    size_t size, length;
    char data[1];
};

#ifndef WIN32
#include <sys/un.h> /* needed for unix sockets */
#endif
#include <time.h>

struct aux_pass {
	http_handler_fn_t http_handler;
#ifdef NO_WEBSOCKETS
	void *ws_connected;
#else
	ws_connected_fn_t ws_connected;
#endif
};	

struct client_connection {
	/* common information for all connections */
	client_connection_info_t info;

	/* private vars for http connections */
	char *line_buf;                /* line buffer (used for request and headers) */
	char *url, *body;              /* URL and request body */
	char *content_type;            /* content type (if set) */
	unsigned int line_pos, body_pos; /* positions in the buffers */
	size_t content_length;           /* desired content length */
	char part, method;             /* request part, method */
	char method8[8];               /* first 7 characters of method */
	int  attr;                     /* connection attributes */
	double req_date;               /* request date */
	char *ws_protocol, *ws_version, *ws_key;
	char *auth;                    /* authorization header (if set) */
	struct buffer *headers;        /* buffer holding header lines */
	struct aux_pass *aux;
	int pipelined;                 /* the line buffer already holds the start of the next request */
};

typedef struct accept_connection {
	/* common information for all connections */
	client_connection_info_t info;
} accept_connection;

#define IS_HTTP_1_1(C) (((C)->attr & HTTP_1_0) == 0)

/* returns the HTTP/x.x string for a given connection - we support 1.0 and 1.1 only */
#define HTTP_SIG(C) (IS_HTTP_1_1(C) ? "HTTP/1.1" : "HTTP/1.0")

/* free buffers starting from the tail(!!) */
static void free_buffer(struct buffer *buf) {
    if (!buf) return;
    if (buf->prev) free_buffer(buf->prev);
    free(buf);
}

/* allocate a new buffer */
static struct buffer *alloc_buffer(size_t size, struct buffer *parent) {
    struct buffer *buf = (struct buffer*) malloc(sizeof(struct buffer) + size);
    if (!buf) return buf;
    buf->next = 0;
    buf->prev = parent;
    if (parent) parent->next = buf;
    buf->size = size;
    buf->length = 0;
    return buf;
}

typedef struct {
	size_t size;
	char buf[1];
} buf_t;

static buf_t *alloc_buf(size_t size) {
	buf_t *buf = (buf_t*) malloc(size + sizeof(buf_t));
	buf->size = size;
	return buf;
}

/* convert doubly-linked buffers into one big raw vector */
static buf_t *collect_buffers(struct buffer *buf) {
    buf_t *res;
    char *dst;
    size_t len = 0;
    if (!buf) return alloc_buf(0);
    while (buf->prev) { /* count the total length and find the root */
		len += buf->length;
		buf = buf->prev;
    }
	len += buf->length;
    res = alloc_buf(len + 1);
    dst = res->buf;
    while (buf) {
		memcpy(dst, buf->data, buf->length);
		dst += buf->length;
		buf = buf->next;
    }
	res->buf[len] = 0; /* guarantee a trailing NUL so it can be used as a string */
    return res;
}

/* resets all allocated structures without touching the connection itself.
   This is used when the request has been answered to prepare for a new one. */
static void conn_request_done(client_connection_t *c) {
	if (c->url) { free(c->url); c->url = NULL; }
	if (c->body) { free(c->body); c->body = NULL; }
	if (c->content_type) { free(c->content_type); c->content_type = NULL; }
	if (c->auth) { free(c->auth); c->auth = NULL; }
	if (c->headers) { free_buffer(c->headers); c->headers = NULL; }
	if (c->ws_key) { free(c->ws_key); c->ws_key = NULL; }
	if (c->ws_protocol) { free(c->ws_protocol); c->ws_protocol = NULL; }
	if (c->ws_version) { free(c->ws_version); c->ws_version = NULL; }
	c->body_pos = 0;
	c->method = 0;
	c->attr = 0;
	c->content_length = 0;
	c->req_date = 0;
	c->part = PART_REQUEST;
}

/* closes the connection */
static void http_close(client_connection_t *conn) {
	if (conn->info.ssl)
		close_tls(conn);
	DBG(ulog_debug("http_close %p (%d)", (void*) conn, conn->info.s));
	if (conn->info.s != INVALID_SOCKET) {
#if 0 /* attempt to avoid TIME_WAIT, but failed */
		char xx[4];
		shutdown(conn->info.s, SHUT_RDWR);
		recv(conn->info.s, xx, 1, 0);
#endif
		closesocket(conn->info.s);
	}
	conn->info.s = INVALID_SOCKET;
}

static void conn_finalize(client_connection_t *c)
{
    DBG(ulog_debug("http: finalizing connection %p", (void*) c));
	/* close the connection */
	http_close(c);
	/* free all the transient variables */
	conn_request_done(c);
	/* then free the rest */
	if (c->line_buf) {
		free(c->line_buf);
		c->line_buf = NULL;
	}
}

static int send_response(client_connection_t *c, const char *buf, size_t len)
{
	server_t *srv = c->info.srv;
    unsigned int i = 0;
    /* we have to tell R to ignore SIGPIPE otherwise it can raise an error
       and get us into deep trouble */
    while (i < len) {
		int send_len = ((len - i) > MAX_SEND_LEN) ? MAX_SEND_LEN : ((int) (len - i));
		int n = srv->send(c, buf + i, send_len);
		if (n < 1) {
			/* FIXME: we're not setup well for blocking sends - we could to this
			   asynchronously by managing the buffer to send, but for now
			   we are fully synchronous even on non-blocking connections */
			if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
				fd_set sfs, efs;
				FD_ZERO(&sfs);
				FD_ZERO(&efs);
				FD_SET(c->info.s, &sfs);
				FD_SET(c->info.s, &efs);
				select(c->info.s + 1, 0, &sfs, &efs, 0);
				/* we don't even bother checking what triggered it, because
				   both will cause send() to return one way or another */
				continue;
			}
			/* FIXME: what about EINTR? */
			return -1;
		}
		i += n;
    }
    return 0;
}

/* sends HTTP/x.x plus the text (which should be of the form " XXX ...") */
static int send_http_response(client_connection_t *c, const char *text) {
    char buf[96];
	server_t *srv = c->info.srv;
    const char *s = HTTP_SIG(c);
    int l = strlen(text), res;
    /* reduce the number of packets by sending the payload en-block from buf */
    if (l < sizeof(buf) - 10) {
		strcpy(buf, s);
		strcpy(buf + 8, text);
		return send_response(c, buf, l + 8);
    }
    res = srv->send(c, s, 8);
    if (res < 8) return -1;
    return send_response(c, text, strlen(text));
}

/* decode URI in place (decoding never expands) */
/* returns non-zero if the input decodes to a NUL byte (which would silently
   truncate the path) */
static int uri_decode(char *s)
{
    int bad = 0;
    char *t = s;
    while (*s) {
		if (*s == '%') { /* NOTE: '+' is literal in paths, only the query uses it for SPC */
			unsigned char ec = 0;
			s++;
			if (*s >= '0' && *s <= '9') ec |= ((unsigned char)(*s - '0')) << 4;
			else if (*s >= 'a' && *s <= 'f') ec |= ((unsigned char)(*s - 'a' + 10)) << 4;
			else if (*s >= 'A' && *s <= 'F') ec |= ((unsigned char)(*s - 'A' + 10)) << 4;
			if (*s) s++;
			if (*s >= '0' && *s <= '9') ec |= (unsigned char)(*s - '0');
			else if (*s >= 'a' && *s <= 'f') ec |= (unsigned char)(*s - 'a' + 10);
			else if (*s >= 'A' && *s <= 'F') ec |= (unsigned char)(*s - 'A' + 10);
			if (*s) s++;
			if (!ec) bad = 1;
			*(t++) = (char) ec;
		} else *(t++) = *(s++);
    }
    *t = 0;
    return bad;
}

/* finalize a request - essentially for HTTP/1.0 it means that
 * we have to close the connection */
static void fin_request(client_connection_t *c) {
    if (!IS_HTTP_1_1(c))
		c->attr |= CONNECTION_CLOSE;
}

void http_free_result(http_result_t* res) {
	if (res->err) free(res->err);
	if (res->payload) free(res->payload);
	if (res->content_type) free(res->content_type);
	if (res->headers) free(res->headers);
}

/* -- logging -- */

static void web_log(http_request_t *req, http_result_t *res) {
	char wlbuf[256];
	const char *c = req->headers ? strstr(req->headers, "X-Real-IP:") : "";
	if (c) {
		const char *d;
		c += 10;
		while (*c == '\t' || *c == ' ') c++;
		d = c;
		while (*d && *d > ' ') d++;
		if (d - c > 64) d = c + 64;
		memcpy(wlbuf, c, d - c);
		wlbuf[d - c] = 0;
	} else /* FIXME: supply actual IP from the connection? */
		*wlbuf = 0;
	ulog_info("HTTP %d\t%s\t%d\t%ld\t%s\t%s", res->code ? res->code : 200, wlbuf, res->payload_type, (long) res->payload_len, req->url, res->err ? res->err : "");
}

/* process a request by calling the httpd() function in R */
static void process_request(client_connection_t *c)
{
    char *query = 0, *fragment = 0, *s;
    DBG(ulog_debug("process request for %p", (void*) c));
    if (!c || !c->url) return; /* if there is not enough to process, bail out */
#ifndef NO_WEBSOCKETS
	if ((c->attr & WS_UPGRADE) && (c->srv->flags & HTTP_WS_UPGRADE)) {
		WS13_upgrade(c, c->ws_key, c->ws_protocol, c->ws_version, c->aux->ws_connected);
		/* the WS swtich messes up args since it replaces it with its own version so
		   we can't go back to serving - just bail out (NOTE: only works when forked!) */
		exit(0);
	}
#endif

	begin_pool("http.process_request");

    s = c->url;
    s = strchr(s, '?'); /* find the query part */
    if (s) {
		*(s++) = 0;
		query = s;
    } else s = c->url;
	s = strchr(s, '#'); /* find the fragment part */
	if (s) {
		*(s++) = 0;
		fragment = s;
	}

    if (uri_decode(c->url)) { /* decode the path part */
	send_http_response(c, " 400 Bad Request (invalid path encoding)\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
	c->attr |= CONNECTION_CLOSE;
	end_pool();
	return;
    }
    {   /* construct "try(httpd(url, query, body, headers), silent=TRUE)" */
		http_request_t req;
		http_result_t res = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
		buf_t *headers = c->headers ? collect_buffers(c->headers) : 0;
		req.url = c->url;
		req.body = c->body;
		req.body_len = c->body_pos;
		req.query = query;
		req.fragment = fragment;
		req.content_type = c->content_type;
		req.headers = headers ? headers->buf : 0;
		req.auth = c->auth;
		req.date = c->req_date;
		memcpy(req.method, c->method8, 7);
		req.method[7] = 0;
		req.processing_flags = 0;
		c->aux->http_handler(&req, &res);
		if (c->info.srv->flags & SRV_LOG)
			web_log(&req, &res);
		if (headers) {
			free(headers);
			headers = 0;
		}
		/* check if the request took ownership via http_request_set_url() */
		if (req.url != c->url)
			c->url = req.url;

		/* the result is expected to have one of the following forms:

		   a) character vector of length 1 => error (possibly from try),
		   will create 500 response
		   
		   b) list(payload[, content-type[, headers[, status code]]])
		   
		   payload: can be a character vector of length one or a
		   raw vector. if the character vector is named "file" then
		   the content of a file of that name is the payload
		   
		   content-type: must be a character vector of length one
		   or NULL (if present, else default is "text/html")
		   
		   headers: must be a character vector - the elements will
		   have CRLF appended and neither Content-Type nor
		   Content-Length may be used
		   
		   status code: must be an integer if present (default is 200)
		*/

		if (res.err) {
			send_http_response(c, " 500 Evaluation error\r\nConnection: close\r\nContent-Type: text/plain\r\n\r\n");
			DBG(ulog_debug("respond with 500 and content: %s", s));
			if (c->method != METHOD_HEAD)
				send_response(c, res.err, strlen(res.err));
			c->attr |= CONNECTION_CLOSE; /* force close */
			http_free_result(&res);
			end_pool();
			return;
		}
		
		{
			int code = res.code ? res.code : 200; /* if no code is provided we assume 200 */
			char buf[64]; /* "HTTP/x.x xxx code_desc[16]\r\nContent-Type: " = 45 */
			const char *ct = res.content_type ? res.content_type : "text/html";
			/* FIXME: for PAYLOAD_[TEMP]FILE the requst can still fail if the file cannot
			   be opened - perhaps defer? */
			if (code < 0 || code > 999)
				code = 500;
			if (code == 200) {
				send_http_response(c, " 200 OK\r\nContent-Type: ");
				send_response(c, ct, strlen(ct));
			} else if (code == 304) { /* not modified - don't need content type */
				sprintf(buf, "%s 304 Not Modified", HTTP_SIG(c));
				send_response(c, buf, strlen(buf));
			} else if (code >= 400 && code <= 406) {
				sprintf(buf, "%s %d %s\r\nContent-Type: ", HTTP_SIG(c), code, http_40x[code - 400]);
				send_response(c, buf, strlen(buf));
				send_response(c, ct, strlen(ct));
			} else {
				sprintf(buf, "%s %d Code %d\r\nContent-Type: ", HTTP_SIG(c), code, code);
				send_response(c, buf, strlen(buf));
				send_response(c, ct, strlen(ct));
			}
			/* the handler may explicitly avoid the generation of the Date: header by setting
			   res.date to a negative value. Value of 0.0 (default) instructs the server to
			   populate it with the current timestamp, other values are explicit dates */
			if (res.date == 0.0)
				res.date = (double) time(0);
			if (res.date < 0.0)
				send_response(c, "\r\n", 2);
			else {
				char date[32];
				sprintf(buf, "\r\nDate: %s\r\n", posix2http(res.date, date));
				send_response(c, buf, strlen(buf));
			}

			if (res.headers) {
				int hlen = strlen(res.headers);
				send_response(c, res.headers, hlen);
				/* the headers have to be terminated - do so if need be */
				if (hlen && res.headers[hlen - 1] != '\n')
					send_response(c, "\r\n", 2);
			}
			if (res.payload_type == PAYLOAD_FILE || res.payload_type == PAYLOAD_TEMPFILE) {
				const char *fn = res.payload;
				if (fn) {
					char *fbuf;
					FILE *f = fopen(fn, "rb");
					long fsz = 0;
					if (!f) {
						/* FIXME: we just pretend that 0-length is ok, but it's not! */
						ulog_warn("attempt to serve '%s', but it cannot be opened", fn);
						send_response(c, "Content-Length: 0\r\n\r\n", 21);
						http_free_result(&res);
						fin_request(c);
						end_pool();
						return;
					}
					if ((res.flags & RF_USE_RANGE) && !(res.flags & RF_IS_SUBSET)) {
						fseek(f, res.range_start, SEEK_SET);
						fsz = res.range_end - res.range_start;
					} else {
						fseek(f, 0, SEEK_END);
						/* FIXME: long is only 32-bit on Windows */
						fsz = ftell(f);
						fseek(f, 0, SEEK_SET);
					}
					sprintf(buf, "Content-Length: %ld\r\n\r\n", fsz);
					send_response(c, buf, strlen(buf));
					if (c->method != METHOD_HEAD) {
						fbuf = (char*) malloc(32768);
						if (fbuf) {
							while (fsz > 0 && !feof(f)) {
								int rd = (fsz > 32768) ? 32768 : fsz;
								if (fread(fbuf, 1, rd, f) != rd) {
									ulog_warn("read error (%s) while reading '%s'",
											  strerror(errno), fn);
									free(fbuf);
									c->attr |= CONNECTION_CLOSE;
									fclose(f);
									/* fn is res.payload, so unlink before freeing it */
									if (res.payload_type == PAYLOAD_TEMPFILE) unlink(fn);
									http_free_result(&res);
									end_pool();
									return;
								}
								send_response(c, fbuf, rd);
								fsz -= rd;
							}
							free(fbuf);
						} else { /* allocation error - get out */
							ulog_error("out of memory when allocating transfer buffer");
							c->attr |= CONNECTION_CLOSE;
							fclose(f);
							if (res.payload_type == PAYLOAD_TEMPFILE) unlink(fn);
							http_free_result(&res);
							end_pool();
							return;
						}
					}
					fclose(f);
					if (res.payload_type == PAYLOAD_TEMPFILE) unlink(fn);
					http_free_result(&res);
					fin_request(c);
					end_pool();
					return;
				}
				sprintf(buf, "Content-Length: 0\r\n\r\n");
				send_response(c, buf, strlen(buf));
				http_free_result(&res);
				fin_request(c);
				end_pool();
				return;
			} else { /* verbatim payload */
				if (res.code == 304) /* 304 may NOT send any body and thus no content-length */
					send_response(c, "\r\n", 2);
				else {
					size_t payload_start = 0, payload_end = res.payload_len;
					if ((res.flags & RF_USE_RANGE) && !(res.flags & RF_IS_SUBSET) && res.range_start <= res.range_end) {
						if (res.range_end <= payload_end)
							payload_end = res.range_end;
						if (res.range_start <= payload_end)
							payload_start = res.range_start;
					}
					sprintf(buf, "Content-Length: %lu\r\n\r\n",
							(unsigned long) (payload_end - payload_start));
					send_response(c, buf, strlen(buf));
					if (c->method != METHOD_HEAD)
						send_response(c, res.payload + payload_start, payload_end - payload_start);
				}
				http_free_result(&res);
				fin_request(c);
				end_pool();
				return;
			} /* no further branches */
		}
    }
    send_http_response(c, " 500 Invalid response from handler.\r\nConnection: close\r\nContent-Type: text/plain\r\n\r\nServer error: invalid response from handler.\r\n");
    c->attr |= CONNECTION_CLOSE; /* force close */
	end_pool();
}

/* this function is called to fetch new data from the client
 * connection socket and process it */
static void http_input_iteration_(client_connection_t *c) {
    int n;
	server_t *srv = c->info.srv;

    DBG(ulog_debug("worker_input_handler, data=%p", (void*) c));
    if (!c) return;

    DBG(ulog_debug("input handler for worker %p (sock=%d, part=%d, method=%d, line_pos=%d)", (void*) c, (int)c->info.s, (int)c->part, (int)c->method, (int)c->line_pos));

	begin_pool("http_input_iteration");

    /* NOTE: if recv reads several requests at once, the first is processed and
     * `pipelined' makes http_input_iteration() parse the rest without waiting
     * for socket data that may never arrive (GET/HEAD only; a request with a
     * body followed by more data still closes the connection). */
    if (c->part < PART_BODY) {
		char *s = c->line_buf;
		/* a previous request left the start of the next one in the buffer,
		   so parse that first instead of waiting for more data */
		int replay = c->pipelined;
		c->pipelined = 0;
		n = replay ? 0 : srv->recv(c, c->line_buf + c->line_pos, LINE_BUF_SIZE - c->line_pos - 1);
		DBG(ulog_debug("[recv n=%d, line_pos=%d, part=%d, errno=%d]", n, c->line_pos, (int)c->part, (n < 0) ? errno : 0));
		if (n < 0) { /* error, scrap this worker */
			/* some cases are benign */
			if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR)
				http_close(c);
			end_pool();
			return;
		}
		if (n == 0 && !replay) { /* connection closed */
			/* complete requests are processed as soon as they are received,
			   so anything left is an incomplete request which must not be
			   passed to the handler */
			http_close(c);
			end_pool();
			return;
		}
		c->line_pos += n;
		c->line_buf[c->line_pos] = 0;
		DBG(ulog_debug("in buffer: {%s}", c->line_buf));
		while (*s) {
			/* ok, we have genuine data in the line buffer */
			if (s[0] == '\n' || (s[0] == '\r' && s[1] == '\n')) { /* single, empty line - end of headers */
				/* --- check request validity --- */
				DBG(ulog_debug(" end of request, moving to body"));
				if (!(c->attr & HTTP_1_0) && !(c->attr & HOST_HEADER)) { /* HTTP/1.1 mandates Host: header */
					send_http_response(c, " 400 Bad Request (Host: missing)\r\nConnection: close\r\n\r\n");
					http_close(c);
					end_pool();
					return;
				}
				if (c->attr & TRANSFER_ENCODING) { /* we cannot decode chunked bodies, so do not misread them as new requests */
					send_http_response(c, " 501 Not Implemented (Transfer-Encoding)\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
					http_close(c);
					end_pool();
					return;
				}
				if (c->attr & BAD_LENGTH) {
					send_http_response(c, " 400 Bad Request (invalid Content-Length)\r\nConnection: close\r\n\r\n");
					http_close(c);
					end_pool();
					return;
				}
				if ((c->attr & CONTENT_LENGTH) && c->content_length) {
					/* FIXME: limit on body size - make it configurable? */
					if (c->content_length < 0 ||  /* we are parsing signed so negative numbers are bad */
						c->content_length > 2147483640 || /* R will currently have issues with body around 2Gb or more, so better to not go there */
						!(c->body = (char*) malloc(c->content_length + 1 /* allocate an extra termination byte */ ))) {
						send_http_response(c, " 413 Request Entity Too Large (request body too big)\r\nConnection: close\r\n\r\n");
						http_close(c);
						end_pool();
						return;
					}
				}
				c->body_pos = 0;
				c->part = PART_BODY;
				if (s[0] == '\r') s++;
				s++;
				/* move the body part to the beginning of the buffer */
				c->line_pos -= s - c->line_buf;
				memmove(c->line_buf, s, c->line_pos);

				/* GET/HEAD or no content length mean no body - we got all we need */
				if (c->method == METHOD_GET || c->method == METHOD_HEAD ||
					!(c->attr & CONTENT_LENGTH) || c->content_length == 0) {

					/* NOTE: technically, only TRACE is not allowed to send body, all others are
					   and should be ignored - but for historical reasons, we just reject
					   GET/HEAD with body anyway */
					if ((c->attr & CONTENT_LENGTH) && c->content_length > 0) {
						send_http_response(c, " 400 Bad Request (GET/HEAD with body)\r\n\r\n");
						http_close(c);
						end_pool();
						return;
					}
					process_request(c);
					if (c->attr & CONNECTION_CLOSE) {
						http_close(c);
						end_pool();
						return;
					}
					/* keep-alive - reset the worker so it can process a new request */
					conn_request_done(c);
					c->pipelined = (c->line_pos > 0);
					end_pool();
					return;
				}

				/* copy body content (as far as available) */
				c->body_pos = (c->content_length < c->line_pos) ? c->content_length : c->line_pos;
				if (c->body_pos) {
					memcpy(c->body, c->line_buf, c->body_pos);
					c->line_pos -= c->body_pos; /* NOTE: we are NOT moving the buffer since non-zero left-over causes connection close */
				}
				/* POST will continue into the BODY part */

				/* NEW: if we don't have the full body, wait for the next iteration to avoid using recv() twice */
				if (c->body_pos < c->content_length) {
					end_pool();
					return;
				}
				/* otherwise we have the full body so the following BODY processing won't require another read() */
				break;
			}
			{
				char *bol = s;
				while (*s && *s != '\r' && *s != '\n') s++;
				/* a trailing CR may be the first half of CRLF: wait for more data */
				if (!*s || (*s == '\r' && !s[1])) { /* incomplete line */
					if (bol == c->line_buf) {
						if (c->line_pos < LINE_BUF_SIZE - 1) { /* one, incomplete line, but the buffer is not full yet (one byte is reserved for the NUL), just return */
							end_pool();
							return;
						}
						/* the buffer is full yet the line is incomplete - we're in trouble */
						send_http_response(c, " 413 Request entity too large\r\nConnection: close\r\n\r\n");
						http_close(c);
						end_pool();
						return;
					}
					/* move the line to the begining of the buffer for later requests */
					c->line_pos -= bol - c->line_buf;
					memmove(c->line_buf, bol, c->line_pos);
					end_pool();
					return;
				} else { /* complete line, great! */
					if (*s == '\r') *(s++) = 0;
					if (*s == '\n') *(s++) = 0;
					DBG(ulog_debug("complete line: {%s}", bol));
					if (c->part == PART_REQUEST) {
						/* --- process request line --- */
						unsigned int rll = strlen(bol); /* request line length */
						char *url = strchr(bol, ' ');
						if (!url || rll < 14 || strncmp(bol + rll - 9, " HTTP/1.", 8)) { /* each request must have at least 14 characters [GET / HTTP/1.0] and have HTTP/1.x */
							send_response(c, "HTTP/1.0 400 Bad Request\r\n\r\n", 28);
							http_close(c);
							end_pool();
							return;
						}
						url++;
						if (!strncmp(bol + rll - 3, "1.0", 3)) c->attr |= HTTP_1_0;
						if (!strncmp(bol, "GET ", 4))  {
							c->method = METHOD_GET;
							strcpy(c->method8, "GET");
						} else if (!strncmp(bol, "POST ", 5)) {
							c->method = METHOD_POST;
							strcpy(c->method8, "POST");
						} else if (!strncmp(bol, "HEAD ", 5)) {
							c->method = METHOD_HEAD;
							strcpy(c->method8, "HEAD");
						} else { /* copy method (up to 7 chars) for custom methods*/
							char *m = c->method8, *me = m + 7, *src = bol;
							while (m < me && (*src && *src != ' '))
								*(m++) = *(src++);
							*m = 0;
						}
						{
							char *mend = url - 1;
							/* we generate a header with the method so it can be passed to the handler */
							if (!c->headers)
								c->headers = alloc_buffer(1024, NULL);
							/* make sure it fits */
							if (c->headers->size - c->headers->length >= 18 + (mend - bol)) {
								if (!c->method) c->method = METHOD_OTHER;
								/* add "Request-Method: xxx" */
								memcpy(c->headers->data + c->headers->length, "Request-Method: ", 16);
								c->headers->length += 16;
								memcpy(c->headers->data + c->headers->length, bol, mend - bol);
								c->headers->length += mend - bol;	
								c->headers->data[c->headers->length++] = '\n';
							}
						}
						if (!c->method) {
							send_http_response(c, " 501 Invalid or unimplemented method\r\n\r\n");
							http_close(c);
							end_pool();
							return;
						}
						bol[strlen(bol) - 9] = 0;
						c->url = strdup(url);
						c->part = PART_HEADER;
						DBG(ulog_debug("parsed request, method=%d, URL='%s'", (int)c->method, c->url));
					} else if (c->part == PART_HEADER) {
						/* --- process headers --- */
						char *k = bol;
						if (!c->headers)
							c->headers = alloc_buffer(1024, NULL);
						if (c->headers) { /* record the header line in the buffer */
							int l = strlen(bol);
							if (l) { /* this should be really always true */
								if (c->headers->length + l + 1 > c->headers->size) { /* not enough space? */
									int fits = c->headers->size - c->headers->length;
									int needs = 2048;
									if (fits) {
										memcpy(c->headers->data + c->headers->length, bol, fits);
										c->headers->length += fits;
									}
									while (l + 1 - fits >= needs) needs <<= 1;
									if (alloc_buffer(needs, c->headers)) {
										c->headers = c->headers->next;
										memcpy(c->headers->data, bol + fits, l - fits);
										c->headers->length = l - fits;
										c->headers->data[c->headers->length++] = '\n';
									}
								} else {
									memcpy(c->headers->data + c->headers->length, bol, l);
									c->headers->length += l;	
									c->headers->data[c->headers->length++] = '\n';
								}
							}
						}
						while (*k && *k != ':') {
							if (*k >= 'A' && *k <= 'Z')
								*k |= 0x20;
							k++;
						}
						if (*k == ':') {
							*(k++) = 0;
							while (*k == ' ' || *k == '\t') k++;
							DBG(ulog_debug("header '%s' => '%s'", bol, k));
							if (!strcmp(bol, "upgrade") && !strcmp(k, "websocket"))
								c->attr |= WS_UPGRADE;
							if (!strcmp(bol, "content-length")) {
								char *ep;
								long cl = strtol(k, &ep, 10);
								while (*ep == ' ' || *ep == '\t') ep++;
								/* the value must be a number and a repeated header must agree */
								if (ep == k || *ep || ((c->attr & CONTENT_LENGTH) && (size_t) cl != c->content_length))
									c->attr |= BAD_LENGTH;
								c->attr |= CONTENT_LENGTH;
								c->content_length = cl;
							}
							if (!strcmp(bol, "content-type")) {
								char *l = k;
								/* lowercase only the part up to ; */
								while (*l && *l != ';') {
									if (*l >= 'A' && *l <= 'Z') *l |= 0x20;
									l++;
								}
								c->attr |= CONTENT_TYPE;
								if (c->content_type) free(c->content_type);
								c->content_type = strdup(k);
								if (!strncmp(k, "application/x-www-form-urlencoded", 33))
									c->attr |= CONTENT_FORM_UENC;
							}
							if (!strcmp(bol, "transfer-encoding"))
								c->attr |= TRANSFER_ENCODING;
							if (!strcmp(bol, "host"))
								c->attr |= HOST_HEADER;
							if (!strcmp(bol, "authorization")) {
								if (c->auth)
									free(c->auth);
								c->auth = strdup(k);
							}
							if (!strcmp(bol, "date"))
								c->req_date = http2posix(k);
							if (!strcmp(bol, "connection")) {
								char *l = k;
								while (*l) { if (*l >= 'A' && *l <= 'Z') *l |= 0x20; l++; }
								if (!strncmp(k, "close", 5))
									c->attr |= CONNECTION_CLOSE;
							}
							if (!strcmp(bol, "sec-websocket-key")) {
								if (c->ws_key) free(c->ws_key);
								c->ws_key = strdup(k);
							}
							if (!strcmp(bol, "sec-websocket-protocol")) {
								if (c->ws_protocol) free(c->ws_protocol);
								c->ws_protocol = strdup(k);
							}
							if (!strcmp(bol, "sec-websocket-version")) {
								if (c->ws_version) free(c->ws_version);
								c->ws_version = strdup(k);
							}
							DBG(ulog_debug(" [attr = %x]", c->attr));
						}
					} /* processing header line */
				} /* complete */
			}
		}
		if (c->part < PART_BODY) {
			/* we end here if we processed a buffer of exactly one line
			   (request line or a header) - so no more to rememeber
			   and off to another iteration */
			c->line_pos = 0;
			end_pool();
			return;
		}
    }

	/* NOTE: we are avoiding two reads in one iteration if we can, so
	   if the header read contained parts of the body, we moved the
	   part into the buffer and returned from the iteration to avoid
	   landing here. Also PART_BODY will only ever get here
	   if there is actual body to be read */
    if (c->part == PART_BODY && c->body) {
		if (c->body_pos < c->content_length) { /* need to receive more ? */
			DBG(ulog_debug("BODY: body_pos=%d, content_length=%ld", c->body_pos, c->content_length));
			n = srv->recv(c, c->body + c->body_pos, c->content_length - c->body_pos);
			DBG(ulog_debug("      [recv n=%d - had %u of %lu; errno=%d]",
						   n, (unsigned)c->body_pos, (unsigned long) c->content_length, (n < 0) ? errno : 0));
			c->line_pos = 0;
			if (n < 0) { /* error, scrap this worker */
				/* some cases are benign */
				if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR)
					http_close(c);
				end_pool();
				return;
			}
			if (n == 0) { /* connection closed before the whole body arrived */
				/* do not process a request with truncated body */
				http_close(c);
				end_pool();
				return;
			}
			c->body_pos += n;
		}
		if (c->body_pos == c->content_length) { /* yay! we got the whole body */
			/* the line buffer can only hold more data if the whole body was in it,
			   i.e., the body is at its beginning and the rest is the next request */
			size_t consumed = c->content_length;
			process_request(c);
			if (c->attr & CONNECTION_CLOSE) {
				http_close(c);
				end_pool();
				return;
			}
			/* keep-alive - reset the worker so it can process a new request */
			if (c->line_pos)
				memmove(c->line_buf, c->line_buf + consumed, c->line_pos);
			conn_request_done(c);
			c->pipelined = (c->line_pos > 0);
			end_pool();
			return;
		}
    }

	/* there used to be code that handled PART_BODY and !body, but that is impossible,
	   because when PART_BODY is set the no-body case is already handled and never gets here.
	   So the only time we get here is when we did a partial read of the body. */
	end_pool();
}

/* processes available input, including requests that were received
   back-to-back (pipelined) and thus already sit in the line buffer */
static void http_input_iteration(client_connection_t *c) {
    if (!c) return;
    do
	http_input_iteration_(c);
    while (c->info.s != INVALID_SOCKET && c->pipelined);
}

#include "async.h"

static async_queue *http_queue;

/* NOTE: we are responsible for freeing conn */
static void HTTP_connected(client_connection_t *conn) {
	if (prepare_child(conn) != 0) { /* parent or error */
		free(conn);
		return;
	}

	begin_pool("HTTP_connected");

	if (!(conn->line_buf = (char*) malloc(LINE_BUF_SIZE))) {
		ulog_error("unable to allocate line buffer");
		end_pool();
		free(conn);
		return;
	}

	conn->aux = (struct aux_pass*) conn->info.srv->aux;

	if ((conn->info.srv->flags & SRV_TLS) && shared_tls(0))
		add_tls(conn, shared_tls(0), 1);

	if ((conn->info.srv->flags & SRV_ASYNC_PROCESS)) {
		DBG(ulog_debug("HTTP_connected: adding connection (%p) to queue", (void*) conn));
		/* we will be called from the accept handler so we are alrady in the critical region */
		if ((conn->info.srv->flags & SRV_ASYNC_ACCEPT))
			async_queue_add_inside(http_queue, conn);
		else
			async_queue_add(http_queue, conn);
		end_pool();
	} else {
		while (conn->info.s != -1)
			http_input_iteration(conn);

		conn_finalize(conn);
		end_pool();
		free(conn);
	}
}

/* --- client API --- */

/* this function MUST be used if any manipulation of the path/query/fragment parts
   of the request is required, because those are NOT allocated but instead pointers
   into the original received string for efficiency. */
int http_request_set_url(http_request_t *req, const char *path, const char *query, const char *fragment) {
	size_t len, path_len, query_len, fragment_len;
	char *new_path, *c, *new_query = 0, *new_fragment = 0;
	DBG(ulog_debug("http:http_request_set_url(%s, %s,%s)",
				  path ? path : "<NULL>", query ? query : "<NULL>", fragment ? fragment : "<NULL>"));
	if (!path)
		path = "";
	len =
		(path_len     = strlen(path)) +
		(query_len    = (query ? strlen(query) : 0)) +
		(fragment_len = (fragment ? strlen(fragment) : 0)) + 3;
	c = new_path = (char*) malloc(len);
	if (!new_path)
		return -1;
	/* NOTE: we cannot free url until we have copied everything, because
	   it is legal to pass previous components */
	memcpy(c, path, path_len + 1);
	c += path_len + 1;
	if (query) {
		new_query = c;
		memcpy(c, query, query_len + 1);
		c += query_len + 1;
	}
	if (fragment) {
		new_fragment = c;
		memcpy(c, fragment, fragment_len + 1);
	}
	/* now we can free the original */
	if (req->url)
		free(req->url);
	req->url = new_path;
	req->query = new_query;
	req->fragment = new_fragment;
	return 0;
}

static void http_server_fin(server_t *srv) {
	if (srv->aux)
		free(srv->aux);
}

/* FIXME: this is a hack - handlers should have a way
   to talk to us - we should not have to deal with this diretly. */
#include "r_inline.h"

static void http_thread_cleanup(async_queue *q, void *par) {
	r_inline_thread_end();
}

int http_start_threads(int n) {
	if (!http_queue) {
		http_queue = async_queue_new();
		if (!http_queue)
			return -1;
		async_thread_add_cleanup(http_queue, http_thread_cleanup, 0);
		async_queue_start_poll_thread(http_queue);
	}
	return async_queue_set_process_threads(http_queue, n);
}

int http_finish_threads() {
	int res = 0;
	if (http_queue) {
		int res = async_queue_finish(http_queue);
		if (!res) { /* on success release the queue */
			async_queue_free(http_queue);
			http_queue = 0;
		} else
			ulog_error("http_finish_threads: queue finish FAILED, not releasing");
	}
	return res;
}

#ifdef NO_WEBSOCKETS
server_t *create_HTTP_server(int port, const char *sock, int flags, http_handler_fn_t handler, void *ws_connected)
#else
server_t *create_HTTP_server(int port, const char *sock, int flags, http_handler_fn_t handler, ws_connected_fn_t ws_connected)
#endif
{
	server_t *srv = create_server(port, sock, 0, flags);
	DBG(ulog_debug("create_HTTP_server(port=%d, sock=%s, flags=0x%x)", port, sock ? sock : "<NONE>", flags));
	if (srv) {
		struct aux_pass *aux = (struct aux_pass*) malloc(sizeof(struct aux_pass));
		if (!aux) return 0;  /* FIXME: release srv? */
		aux->http_handler = handler;
		aux->ws_connected = ws_connected;
		srv->connected = HTTP_connected;
		/* srv->send_resp = */
		srv->recv      = server_recv;
		srv->send      = server_send;
		srv->fin       = http_server_fin; /* frees aux */
		srv->conn_fin  = conn_finalize;
		srv->process   = http_input_iteration;
		srv->aux       = aux;

		if (flags & (SRV_ASYNC_ACCEPT | SRV_ASYNC_PROCESS)) {
			if (!http_queue && http_start_threads(1) < 1) {
				ulog_error("cannot create HTTP processing queue");
				free(aux);
				free(srv);
				return 0;
			}
			if (flags & SRV_ASYNC_ACCEPT) {
				/* create connection (and its server) for the accept stage */
				server_t *asrv = create_accept_server(srv);
				accept_connection *aconn = (accept_connection*) calloc(sizeof(accept_connection), 1);
				aconn->info.s = srv->ss;
				aconn->info.srv = asrv;
				add_server(srv);
				async_queue_add(http_queue, (client_connection_t*) aconn);
			} else
				add_server(srv);
		} else
			add_server(srv);
		return srv;
	}
	return 0;
}

/*--- The following makes the indenting behavior of emacs compatible
      with Xcode's 4/4 setting ---*/
/* Local Variables: */
/* indent-tabs-mode: t */
/* tab-width: 4 */
/* c-basic-offset: 4 */
/* End: */
