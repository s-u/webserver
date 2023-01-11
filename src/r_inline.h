#ifndef R_INLINE_H__
#define R_INLINE_H__

#include "http.h"

#include <Rinternals.h>

typedef struct r_inline r_inline;

void r_inline_free(r_inline *obj);
r_inline *r_inline_entry(const char *prefix, SEXP sFun, int flags);
int r_inline_handler(http_request_t *req, http_result_t *res, void *user);

#endif
