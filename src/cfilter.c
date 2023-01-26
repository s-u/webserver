#include "cfilter.h"

struct conn_filter {
    struct conn_filter *next;
    int (*filter)(client_connection_info_t *ci, void *par);
    void (*fin)(void *par);
    void *par;
};

conn_filter *conn_filter_add(conn_filter *prev, /* can be NULL */
			     int (*filter)(client_connection_info_t *ci, void *par), /* filter callback */
			     void (*fin)(void *par), /* finalizer for par - can be NULL */
			     void *par) { /* user data (can be NULL) */
    conn_filter *f;
    if (!filter)
	return 0;
    f = (conn_filter*) malloc(sizeof(conn_filter));
    if (!f)
	return f;
    f->next = 0;
    f->filter = filter;
    f->fin = fin;
    f->par = par;
    f->next = prev;
    return f;
}

int conn_filter_allowed(conn_filter *f, client_connection_info_t *ci) {
    if (!f)
	return 1;
    while (f) {
	if (f->filter(ci, f->par))
	    return 1;
	f = f->next;
    }
    return 0;
}

void conn_filter_free(conn_filter *f) {
    while (f) {
	conn_filter *me = f;
	if (f->fin)
	    f->fin(f->par);
	f = f->next;
	free(me);
    }
}

#if 0 /* IPv4 filter ... */
/* par = NULL terminated list of IP addresses (char*) */
int conn_filter_allowed_ipv4(client_connection_info_t *ci, void *par) {
    char **laddr = (char**) pars;
    int allowed = 0;
    while (*laddr) {
	if (ci->sa.ip4.sin_family == AF_INET &&
	    ci->sa.ip4.sin_addr.s_addr == inet_addr(*laddr))
	    {
		allowed=1;
		break;
	    }
	laddr++;
    }
    return allowed;
}
#endif
