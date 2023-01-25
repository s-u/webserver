#ifndef R_EXT_H__
#define R_EXT_H__

#include "http.h"
#include "r_inline.h"

client_connection_t *r_ext_new_worker(r_inline *ri);

int  r_ext_one_request(client_connection_t *c, http_request_t *req, http_result_t *res);
/* return 1 if the worker is connected and ready to accept a request */
int  r_ext_ready(client_connection_t *c);

void r_ext_close(client_connection_t *c);

#endif
