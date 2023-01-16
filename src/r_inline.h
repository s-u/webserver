#ifndef R_INLINE_H__
#define R_INLINE_H__

#include "http.h"

#include <Rinternals.h>

/* this is not really a flag for the handler, but rather only used
   in r_inline_setup() */
#define RIF_ASYNC           1

/* the following are handler flags */
#define RIF_PARSE_QUERY     2 /* if set, query is a named character vector, otherwise just unparsed string */
#define RIF_PARSE_HEADERS   4 /* if set, headers are parsed into pairlists, otherwise raw vector */

typedef struct r_inline r_inline;

void r_inline_free(r_inline *obj);
r_inline *r_inline_entry(const char *prefix, SEXP sFun, int flags);
int r_inline_handler(http_request_t *req, http_result_t *res, void *user);

void r_inline_setup(int flags);
void r_inline_end();

#endif
