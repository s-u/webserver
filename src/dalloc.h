#ifndef DALLOC_H__
#define DALLOC_H__

#ifndef NO_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>

#ifdef ALLOC_REPORT_ALL
#ifndef ALLOC_REPORT
#define ALLOC_REPORT
#endif
#endif
#ifdef ALLOC_REPORT
#ifndef TRACK_ALLOC
#define TRACK_ALLOC
#endif
#endif

#ifdef TRACK_ALLOC

typedef struct allocation_pool allocation_pool; 

allocation_pool *d_new_pool(const char *region, const char *fn, int line);
void *d_malloc(allocation_pool *ap, size_t size, const char *fn, int line);
void *d_calloc(allocation_pool *ap, size_t count, size_t size, const char *fn, int line);
void  d_free(allocation_pool *ap, void *ptr, const char *fn, int line);
char *d_strdup(allocation_pool *ap, const char *s, const char *fn, int line);
void  d_end_pool(allocation_pool *ap, const char *fn, int line);
void  d_report_pool(allocation_pool *ap, const char *fn, int line);


#ifdef malloc
#undef malloc
#endif
#define malloc(X) d_malloc(ALLOC_POOL, X, __FILE__, __LINE__)
#ifdef calloc
#undef calloc
#endif
#define calloc(X, Y) d_calloc(ALLOC_POOL, X, Y, __FILE__, __LINE__)
#ifdef strdup
#undef srdup
#endif
#define strdup(X) d_strdup(ALLOC_POOL, X, __FILE__, __LINE__)
#ifdef free
#undef free
#endif
#define free(X) d_free(ALLOC_POOL, X, __FILE__, __LINE__)

#define ALLOC_POOL alloc_pool_
#define begin_pool(X) allocation_pool *alloc_pool_ = d_new_pool(X, __FILE__, __LINE__)
#define report_pool() d_report_pool(alloc_pool_, __FILE__, __LINE__)
#define end_pool() d_end_pool(alloc_pool_, __FILE__, __LINE__)
#define alloc_global_report() d_report_pool(NULL, __FILE__, __LINE__)

/* per-module fall-back allocation pool */
static allocation_pool *alloc_pool_;

#else

#define begin_pool(X) while(0) {}
#define end_pool() while(0) {}
#define report_pool() while(0) {}
#define alloc_global_report() while(0) {}

#endif

#endif
