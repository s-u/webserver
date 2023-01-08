/* Simple rewrite HTTP handler.

   (C)Copyright 2022 by Simon Urbanek <simon.urbanek@R-project.org>
   License: MIT

*/

#include "ulog.h"
#include "http.h"
#include "http_tools.h"
#include "http_date.h"
#include "http_rewrite.h"

#include <string.h>
#include <stdio.h>

typedef struct http_rewrite {
    char *prefix, *replace;
    int placeholders, prefix_len, replace_len;
    int flags;
    char *pos[1];
} http_rewrite;

http_rewrite *http_rewrite_entry(const char *prefix, const char *replace, int flags) {
    http_rewrite *hs;
    int n = 0;
    const char *c = prefix;
    if (!prefix || !replace)
	return 0;

    while (*c) {
	if (*c == '?')
	    n++;
	c++;
    }

    hs = (http_rewrite*) calloc(1, sizeof(http_rewrite) + n * (sizeof(char*) * 2));
    if (!hs)
	return 0;
    if (!(hs->prefix = strdup(prefix)))
	return 0;
    if (!(hs->replace = strdup(replace))) {
	free(hs->prefix);
	return 0;
    }
    hs->placeholders = n;
    hs->flags = flags;
    /* cached for performance */
    hs->prefix_len = strlen(prefix);
    hs->replace_len = strlen(replace);
    return hs;
}

int http_rewrite_handler(http_request_t *req, http_result_t *res, void *user) {
    http_rewrite *r = (http_rewrite*) user;
    const char *path;

    /* this should never happen, but just to be safe ... */
    if (!req || !req->url || !r) return 0;

    path = req->url;
    DBG(ulog_debug("rewrite: match '%s' and '%s'", path, r->prefix));
    if (!r->placeholders) { /* fixed rewrite */
	int n;
	char *s;
	if (strncmp(r->prefix, path, r->prefix_len))
	    return 0;
	DBG(ulog_debug(" - matched, fixed rewrite"));
	n = strlen(path + r->prefix_len);
	s = malloc(r->replace_len + n + 1);
	if (!s) {
	    res->code = 500;
	    return 1;
	}
	memcpy(s, r->replace, r->replace_len);
	memcpy(s, path + r->prefix_len, n + 1);
	DBG(ulog_debug(" - replacing '%s' with '%s'", req->url, s));
	http_request_set_url(req, s, req->query, req->fragment);
	/* FIXME: we want other handlers to work, but we also want to have a way
	   to skip other rewrites ...
	   We can't return 1 since that would skip *all* handlers including
	   content handlers, so we return 0 for now, but need some better
	   solution ... */
	return 0;
    }
    return 0;
}
