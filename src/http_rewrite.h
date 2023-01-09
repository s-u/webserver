#ifndef HTTP_REWRITE_H__
#define HTTP_REWRITE_H__

#include "http.h"

/* request this rule to be the last rewrite rule if matched */
#define REWRITE_FLAG_LAST 1

typedef struct http_rewrite http_rewrite;

http_rewrite *http_rewrite_entry(const char *prefix, const char *replace, int flags);
int http_rewrite_handler(http_request_t *req, http_result_t *res, void *user);

#endif
