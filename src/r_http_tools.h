#ifndef R_HTTP_TOOLS_H__
#define R_HTTP_TOOLS_H__

#include <Rinternals.h>

/* if len_spec < 0 then strlen() is used, otherwise hdrs does not need to be NUL terminated */
SEXP r_http_parse_headers(const char *hdrs, int len_spec);

SEXP r_http_parse_query(char *query);

#endif
