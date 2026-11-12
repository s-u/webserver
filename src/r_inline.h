#ifndef R_INLINE_H__
#define R_INLINE_H__

#include "http.h"

#include <Rinternals.h>

/* this is not really a flag for the handler, but rather only used
   in r_inline_setup() */
#define RIF_ASYNC           0x20
/* if set uses forked workers */
#define RIF_FORK            0x40

/* the following are handler flags */
#define RIF_PARSE_QUERY     2 /* if set, query is a named character vector, otherwise just unparsed string */
#define RIF_PARSE_HEADERS   4 /* if set, headers are parsed into pairlists, otherwise raw vector */

typedef struct r_inline r_inline;

void r_inline_free(r_inline *obj);
r_inline *r_inline_entry(const char *prefix, SEXP sFun, int flags);
int r_inline_handler(http_request_t *req, http_result_t *res, void *user);

/* the inner part of the handler which does the R call - converting
   req into a call of sFun and populating res with the result.
   NOTE: this function does NOT perform any locking. */
void r_inline_process(http_request_t *req, http_result_t *res, SEXP sFun, int flags);

void r_inline_setup(int flags);
void r_inline_thread_end(void);
void r_inline_end(void);

/* entering R from the main thread, see r_inline.c */
int  r_inline_try_lock(void);
void r_inline_unlock(void);

/* fork support for r_ext workers, see r_inline.c */
void r_inline_fork_lock(void);
void r_inline_fork_unlock(void);
void r_inline_fork_child(void);

#endif
