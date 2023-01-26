#ifndef CFILTER_H__
#define CFILTER_H__

#include "server.h"

typedef struct conn_filter conn_filter;

conn_filter *conn_filter_add(conn_filter *prev, /* can be NULL */
			     int (*filter)(client_connection_info_t *ci, void *par), /* filter callback */
			     void (*fin)(void *par), /* finalizer for par - can be NULL */
			     void *par); /* user data (can be NULL) */

/* NOTE: no filter (f = NULL) -> all connections are allowed */
int conn_filter_allowed(conn_filter *f, client_connection_info_t *ci);

void conn_filter_free(conn_filter *f);

#endif
