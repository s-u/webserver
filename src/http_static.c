/* Static content HTTP handler.

   (C)Copyright 2022 by Simon Urbanek <simon.urbanek@R-project.org>
   License: MIT

*/

#include "ulog.h"
#include "http.h"
#include "http_tools.h"
#include "http_date.h"
#include "http_static.h"
#include <sys/stat.h>
#define  NEED_STRNSTR
#include "bsdcmpt.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

typedef struct http_static {
    struct http_static *next, *prev;
    char *prefix, *path, *index;
    int prefix_len;
    int flags;
} http_static;

void http_static_free(http_static *hs) {
    if (!hs)
	return;
    if (hs->prefix)
	free(hs->prefix);
    if (hs->path)
	free(hs->path);
    if (hs->index)
	free(hs->index);
    free(hs);
}

http_static *http_static_entry(const char *prefix, const char *path,
			       const char *index, int flags) {
    http_static *hs;
    if (!prefix || !path)
	return 0;

    hs = (http_static*) calloc(1, sizeof(http_static));
    if (!hs)
	return 0;
    if (!(hs->prefix = strdup(prefix)))
	return 0;
    if (!(hs->path = strdup(path))) {
	free(hs->prefix);
	return 0;
    }
    if (index && !(hs->index = strdup(index))) {
	free(hs->prefix);
	free(hs->path);
	return 0;
    }
    hs->flags = flags;
    hs->prefix_len = strlen(prefix); /* cached for performance */

    return hs;
}

int http_static_handler(http_request_t *req, http_result_t *res, void *user) {
    struct stat st;
    double ts;
    const char *append_headers = 0, *c_type = 0, *fn, *path;
    http_static *hs = (http_static*) user;
    int found = 0;
    /* buffer used for path processing in the handler. */
    char http_tmp[512];

    /* this should never happen, but just to be safe ... */
    if (!req || !req->url || !hs) return 0;

    begin_pool("http_static_handler");

    path = req->url;
    DBG(ulog_debug("match '%s' and '%s'", path, hs->prefix));
    if (strncmp(hs->prefix, path, hs->prefix_len)) {
	end_pool();
	return 0;
    }

    fn = path + hs->prefix_len;
    if (strlen(fn) + strlen(hs->path) +
	(hs->index ? strlen(hs->index) : 0) + 7 > sizeof(http_tmp)) {
	res->code = 414;
	res->err = strdup("Path too long.");
	end_pool();
	return 1;
    }
    strcpy(http_tmp, hs->path);
    strcat(http_tmp, fn);

    DBG(ulog_debug(" - matched, trying '%s'", http_tmp));

    if (!stat(http_tmp, &st)) { /* path exists */
	DBG(ulog_debug(" - path exists, mode: 0x%x", st.st_mode));
	if (st.st_mode & S_IFDIR) { /* if it is a directory, we only accept it if index exists */
	    if (hs->index) {
		strcat(http_tmp, hs->index);
		DBG(ulog_debug(" - trying '%s'\n", http_tmp));
		if (!stat(http_tmp, &st))
		    found = 1;
	    }
	} else if (st.st_mode & S_IFREG) /* otherwise we only accept regular files */
	    found = 1;
	else DBG(ulog_debug(" - found, but neither file not directory -> 404"));
    }
	
    /* not found + stop = 404 */
    if (!found) {
	end_pool();
	if (hs->flags & HSF_STOP) {
	    res->code = 404;
	    return 1;
	} else /* otherwise go to the next handler */
	    return 0;
    }

    /* check for conditional GET */
    if (req->headers) {
	const char *if_mod = get_header(req, "if-modified-since");
	if (if_mod) {
	    double since = http2posix(if_mod);
	    if (since >= MTIME(st)) {
		/* the standard requires only Cache-Control (and ETag if used),
		   but suggests Last-Modified if there is no ETag */
		snprintf(http_tmp, sizeof(http_tmp),
			 "Last-Modified: %s\r\nCache-Control: no-cache\r\n%s",
			 posix2http(MTIME(st)),
			 append_headers ? append_headers : "");
		res->headers = strdup(http_tmp);
		res->code = 304;
		end_pool();
		return 1;
	    }
	}
    }

    c_type = infer_content_type(http_tmp);

    /* check whether the client is ok with gzip compressed version and use that if possible */
    if (hs->flags & HSF_PRECOMPRESSED) {
	const char *accept = get_header(req, "accept-encoding");
	if (accept) {
	    const char *e = strchr(accept, '\n');
	    if (e && strnstr(accept, "gzip", e - accept)) { /* accepts gzip? */
		struct stat gzst;
		char *ext = http_tmp + strlen(http_tmp);
		strcpy(ext, ".gz");
		/* .gz present and not older than the source = serve the compressed version */
		if (!stat(http_tmp, &gzst) && MTIME(gzst) >= MTIME(st)) {
		    DBG(ulog_debug(" - serving compressed version: %s", http_tmp));
		    st = gzst;
		    append_headers = "Content-Encoding: gzip\r\n";
		} else /* strip the .gz again if we can't use it */
		    *ext = 0;
	    }
	}
    }

    /* otherwise this was handled and http_tmp has the full name */
    res->payload = strdup(http_tmp);
    res->payload_type = PAYLOAD_FILE;
    if (c_type)
	res->content_type = strdup(c_type);

    /* append Last-Modified: based on the served file and set no-cache */
    ts = (double) time(0);
    snprintf(http_tmp, sizeof(http_tmp), "Last-Modified: %s\r\nCache-Control: no-cache\r\n%s",
             posix2http((MTIME(st) > ts) ? ts : MTIME(st)),
             append_headers ? append_headers : ""
        );
    res->headers = strdup(http_tmp);
    end_pool();
    return 1;
}
