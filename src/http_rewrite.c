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
    char *prefix, *replace, *query;
    int placeholders, prefix_len, replace_len, query_has_ph;
    int flags;
    const char *pos[1];
} http_rewrite;

http_rewrite *http_rewrite_entry(const char *prefix, const char *replace, int flags) {
    http_rewrite *hs;
    int n = 0;
    const char *c = prefix;
    if (!prefix || !replace)
	return 0;

    /* NOTE: by using ? in the syntax we're not allowing query matches, which is
       ok for usual REST routing, but there may be cases where one might want to
       do that ... */
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
    hs->query = hs->replace ? strchr(hs->replace, '?') : 0;
    if (hs->query) /* terminate path and set query */
	*(hs->query++) = 0;
    hs->placeholders = n;
    hs->query_has_ph = (hs->query && strchr(hs->query, '$')) ? 1 : 0;
    hs->flags = flags;
    /* cached for performance */
    hs->prefix_len = strlen(prefix);
    hs->replace_len = strlen(replace);
    return hs;
}

/* replaces $1..$9 with the values stored in r->pos[2*n], writing the
   result to dst.
   dst can be NULL in which case it will return the needed size */
static int do_replace(http_rewrite *r, const char *src, char *dst) {
    int n = 0;
    while (*src) {
	if (*src == '$') {
	    src++;
	    if (*src >= '1' && *src <= '9') {
		int off = *(src++) - '1', rl = 0;
		if (off < r->placeholders)
		    rl = (int) (r->pos[2*off + 1] - r->pos[2*off]);
		if (rl) {
		    if (dst) {
			memcpy(dst, r->pos[2*off], rl);
			dst += rl;
		    }
		    n += rl;
		}
	    } else {
		if (dst)
		    *(dst++) = '$';
		n++;
	    }
	} else {
	    if (dst)
		*(dst++) = *(src++);
	    else
		src++;
	    n++;
	}
    }
    return n;
}

int http_rewrite_handler(http_request_t *req, http_result_t *res, void *user) {
    http_rewrite *r = (http_rewrite*) user;
    const char *path;

    /* this should never happen, but just to be safe ... */
    if (!req || !req->url || !r) return 0;
    /* previous rule has requested no more rewrites */
    if (req->processing_flags & PF_REWRITE_STOP)
	return 0;
    path = req->url;
    DBG(ulog_debug("rewrite: match '%s' and '%s'", path, r->prefix));

    int match_length, replace_length;

    if (!r->placeholders) { /* fixed rewrite - simple case */
	if (strncmp(r->prefix, path, r->prefix_len))
	    return 0;
	DBG(ulog_debug(" - matched, fixed rewrite"));

	match_length = r->prefix_len;
	replace_length = r->replace_len;
    } else { /* placeholder replace */
	int n = 0;
	const char *src = path, *rule = r->prefix;
	/* match and record placeholder value locations */
	while (*src && *rule) {
	    if (*rule == '?') {
		r->pos[2*n] = src;
		while (*src && *src != '/') src++;
		r->pos[2*n + 1] = src;
		rule++;
		/* we counted ? so this will not overflow */
		n++;
		continue;
	    } else if (*(src++) != *(rule++))
		return 0; /* mismatch */
	}
	if (*rule) /* rule was longer = not a match */
	    return 0;
	/* no large integers, so safe to use int */
	match_length = (int) (src - path);

	/* if we got here, it was a match */
	/* need to find the replacement size */
	replace_length = do_replace(r, r->replace, 0);
    }

    int n, al;
    char *new_path, *dst, *qs = 0, *new_query = req->query;

    /* length of the remainder of the path */
    al = n = strlen(path + match_length);
    if (r->query && req->query) /* replacement also needs a new query */
	al += strlen(r->query) + strlen(req->query) + 3;
    new_path = dst = malloc(replace_length + al + 1);
    if (!dst) {
	res->code = 500;
	return 1;
    }

    if (!r->placeholders) { /* fixed rewrite - simple case */    
	/* copy replacement path */
	memcpy(dst, r->replace, r->replace_len);
	dst += r->replace_len;
    } else /* the rule contains placeholders */
	dst += do_replace(r, r->replace, dst);

    /* append non-replaced part of the path + its NUL */
    memcpy(dst, path + match_length, n + 1);
    if (r->query && req->query) { /* concatenate queries - request query goes first */
	new_query = qs = dst + n + 1;
	strcpy(qs, req->query);
	qs += strlen(req->query);
	if (qs > new_query)
	    *(qs++) = '&';
    } else if (r->query_has_ph)
	new_query = qs = dst + n + 1;

    if (r->query_has_ph) { /* we have to also interpret $x in the query replacement part */
	qs += do_replace(r, r->query, qs);
	*qs = 0;
    } else if (r->query && req->query)
	strcat(qs, r->query);

    if (!new_query) /* it starts with req->query */
	new_query = r->query;
    DBG(ulog_debug(" - replacing '%s' with '%s%s%s'", req->url, new_path, new_query ? "?" : "", new_query ? new_query : ""));
    http_request_set_url(req, new_path, new_query ? new_query : r->query, req->fragment);
    free(new_path);

    if (r->flags & REWRITE_FLAG_LAST)
	req->processing_flags |= PF_REWRITE_STOP;
	/* we cannot signal 1 since that would stop actual serving handlers */
	return 0;
    return 0;
}
