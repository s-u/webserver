/* do NOT include dalloc.h since it overrides system allocators */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define F_STRING 1
#define F_FREED  2
#define F_A_FREE 4

typedef struct allocation_pool {
    const char *region;
    struct allocation *root;
    struct allocation_pool *next, *prev;
    size_t allocs, frees, n_alloc, n_free;
} allocation_pool;

#ifdef LOCAL_ALLOC
typedef struct alloc_stack {
    struct alloc_stack *next, *prev;
    size_t a_next, a_last, size;
    char data[1];
} alloc_stack;
#endif

typedef struct allocation {
    allocation_pool *pool;
    struct allocation *next, *prev;
#ifdef TRACK_ALL_ALLOC
    struct allocation *abs_next;
#endif
#ifdef LOCAL_ALLOC
    struct allocation *a_prev;
    alloc_stack *owner;
    size_t a_size;
#endif
    size_t size;
    int flags, line;
    const char *fn;
    char data[1];
} allocation;

#define offsetof(st, m) \
    ((size_t)((char *)&((st *)0)->m - (char *)0))

static allocation *ptr2alloc(void *ptr) {
    char *c = (char*) ptr;
    c -= offsetof(allocation, data);
    return (allocation*) c;
}

static allocation_pool *global, *last_pool;
static size_t g_n_alloc, g_allocs, g_n_free, g_frees, g_cur_size, g_max_size;
#ifdef TRACK_ALL_ALLOC
static allocation *alloc_head, *alloc_tail;
#endif

allocation_pool *d_new_pool(const char *region, const char *fn, int line);

#ifdef LOCAL_ALLOC
static alloc_stack *a_stack;

static alloc_stack *new_stack(size_t size) {
    alloc_stack *as = (alloc_stack*) malloc(sizeof(alloc_stack) + size);
    as->size = size;
    as->next = as->prev = 0;
    as->a_next = as->a_last = 0;
    return as;
}

static allocation *imp_malloc(size_t len) {
    allocation *al;
    if (len & 7) /* always align at 64 bits */
	len = (len - (len & 7)) + 8;
    if (!a_stack)
	a_stack = new_stack(64*1024*1024);
    if (a_stack->size - a_stack->a_next < len) {
#ifdef ALLOC_SINGLE
	/* for debugging purpose and memory pressure tests */
	fprintf(stderr, "ERROR: stack filled, emulating out of memory condition\n");
	return 0;
#else
	if (!(a_stack->next = new_stack(a_stack->size)))
	    return 0;
	a_stack->next->prev = a_stack;
	a_stack = a_stack->next;
#endif
    }
    al = (allocation*) (a_stack->data + a_stack->a_next);
    al->a_prev = a_stack->a_next ? ((allocation*)(a_stack->data + (a_stack->a_last))) : 0;
    a_stack->a_last = a_stack->a_next;
    a_stack->a_next += len;
    al->owner = a_stack;
    al->a_size = len;
#ifdef ALLOC_DEBUG
    fprintf(stderr, "imp_malloc: %p .. %p (%ld .. %ld, %ld)\n",
	    (void*) al, (void*) (((char*)al) + al->a_size), (long) a_stack->a_last, (long) a_stack->a_next, (long) len);
#endif
    return al;
}

static void imp_free(void *ptr) {
    allocation *al = (allocation*) ptr, *cal;
    alloc_stack *as = al->owner;
    size_t my_pos = ((char*) ptr) - as->data, pos = my_pos + al->a_size;
#ifdef ALLOC_DEBUG
    fprintf(stderr, "imp_free(%p, %ld, %ld) [stack=%ld->%ld] ", ptr, (long) my_pos, (long) al->a_size, (long) as->a_last, (long) as->a_next);
#endif
    /* check if all entries behind us are free */
    while (pos < as->a_next && ((cal = (allocation*)(as->data + pos))->flags & F_A_FREE)) {
#ifdef ALLOC_DEBUG
	fprintf(stderr, "<%ld->%ld is free> ", pos, pos + cal->a_size);
#endif
	pos += cal->a_size;
    }
    if (pos < as->a_next) { /* cannot free, just flag */
#ifdef ALLOC_DEBUG
	fprintf(stderr, "(%ld and has non-free neighbor at %ld)\n", (long) my_pos, (long) pos);
#endif
	al->flags |= F_A_FREE;
    } else {
	/* see if we were blocking other releases */
	while (al->a_prev && (al->a_prev->flags & F_A_FREE))
	    al = al->a_prev;
	if (!al->a_prev) { /* everything got released */
	    as->a_next = as->a_last = 0;
#ifdef ALLOC_DEBUG
	    fprintf(stderr, "(leads to empty stack)\n");
#endif
	} else {
	    as->a_last = ((char*)(al->a_prev)) - as->data;
	    as->a_next = as->a_last + al->a_prev->a_size;
#ifdef ALLOC_DEBUG
	    fprintf(stderr, "(full release %ld, last=%ld, next=%ld)\n", (long) my_pos, (long) as->a_last, (long) as->a_next);
#endif
	}
    }
}
#else
#define imp_malloc malloc
#define imp_free free
#endif

void *d_malloc(allocation_pool *ap, size_t size, const char *fn, int line) {
    allocation *al = (allocation*) imp_malloc(sizeof(allocation) + size);
    if (!al)
	return 0;
    if (!ap) {
	if (!global)
	    global = d_new_pool(NULL, fn, line);
	ap = last_pool ? last_pool : global;
#ifdef ALLOC_REPORT_ALL
	fprintf(stderr, "NOTE: allocation without a pool (%ld bytes, %s:%d) using %s\n",
		(long) size, fn, line, ap->region);
#endif
    }
#ifdef ALLOC_REPORT_ALL
    fprintf(stderr, "INFO: alloc %p (%ld, %s:%d) %s\n", (void*) al->data, (long) size, fn, line, ap->region);
#endif
    al->pool = ap;
    al->next = al->prev = 0;
    al->size = size;
    al->flags = 0;
    al->line = line;
    al->fn = fn;
    ap->allocs += size;
    ap->n_alloc++;
    g_allocs += size;
    g_n_alloc++;
    g_cur_size += size;
    if (g_cur_size > g_max_size)
	g_max_size = g_cur_size;
    if (ap->root) {
	al->next = ap->root;
	ap->root->prev = al;
    }
    ap->root = al;
#ifdef TRACK_ALL_ALLOC
    if (!alloc_head)
	alloc_head = alloc_tail = al;
    else {
	alloc_tail->abs_next = al;
	alloc_tail = al;
    }
#endif
	
    return al->data;
}

void  d_free(allocation_pool *ap, void *ptr, const char *fn, int line) {
    allocation *al = ptr2alloc(ptr), *pal = al;
    int depth = 0;
    while ((pal = pal->prev))
	depth++;

#ifdef ALLOC_REPORT_ALL
    if (!ap)
	fprintf(stderr, "NOTE: freeing %p %s (%ld bytes) without pool, assigned to %s (%s:%d)\n",
		ptr, (al->flags & F_STRING) ? ((char*)ptr) : "", al->size, al->pool->region, fn, line);
    else if (al->pool != ap)
	fprintf(stderr, "NOTE: freeing %p %s (%ld bytes) from %s but was allocated in %s (%s:%d)\n",
		ptr, (al->flags & F_STRING) ? ((char*)ptr) : "", al->size, ap->region, al->pool->region, fn, line);
#endif
    ap = al->pool;
    if (al->next)
	al->next->prev = al->prev;
    if (al->prev)
	al->prev->next = al->next;
    if (ap->root == al)
	ap->root = al->next;
    ap->frees += al->size;
    ap->n_free++;
    g_frees += al->size;
    g_n_free++;
    g_cur_size -= al->size;
#ifdef ALLOC_REPORT_ALL
    fprintf(stderr, "INFO: free %p (%ld, %s:%d, depth %d) %s\n", (void*) al->data, (long) al->size, fn, line, depth, ap->region);
#endif
#ifdef TRACK_ALL_ALLOC
    al->flags |= F_FREED;
#else
    imp_free(al);
#endif
}

void *d_calloc(allocation_pool *ap, size_t count, size_t size, const char *fn, int line) {
    size_t n = size * count;
    void *a = d_malloc(ap, n, fn, line);
    if (a)
	memset(a, 0, n);
    return a;
}

char *d_strdup(allocation_pool *ap, const char *s, const char *fn, int line) {
    size_t n = s ? strlen(s) : 0;
    char *a = (char*) d_malloc(ap, n + 1, fn, line);
    if (a) {
	memcpy(a, s, n + 1);
	ptr2alloc(a)->flags |= F_STRING;
    }
    return a;
}

allocation_pool *d_new_pool(const char *region, const char *fn, int line) {
    allocation_pool *ap = calloc(sizeof(allocation_pool), 1);
    if (!global) {
	last_pool = global = ap;
	ap->region = "<global>";
	if (!region) /* special case - we use NULL to force global pool allocation */
	    return ap;
	ap = calloc(sizeof(allocation_pool), 1);
    }
    ap->region = region;
    last_pool->next = ap;
    ap->prev = last_pool;
    last_pool = ap;
#ifdef ALLOC_REPORT_ALL
    fprintf(stderr, "INFO: new allocation pool %s (%s:%d)\n", region, fn, line);
#endif
    return ap;
}

void  d_report_pool(allocation_pool *ap, const char *fn, int line) {
    allocation *al;
    if (!ap) {
	if (global) {
	    d_report_pool(global, fn, line);
	    fprintf(stderr, "Global totals:\n - allocated %ld bytes, %ld objects.\n - freed %ld bytes, %ld objects.\n - delta: %ld bytes, %ld objects\n - max memory usage: %ld\n------\n",
		    (long) g_allocs, (long) g_n_alloc,
		    (long) g_frees, (long) g_n_free,
		    ((long) g_allocs) - ((long) g_frees),
		    ((long) g_n_alloc) - ((long) g_n_free),
		    (long) g_max_size);
#ifdef TRACK_ALL_ALLOC
	    {
		allocation *al = alloc_head;
		while (al) {
		    if (!(al->flags & F_FREED))
			fprintf(stderr, "LEAKING: %p (%ld) %s (allocated at %s:%d)\n",
				(void*) al->data, (long) al->size, (al->flags & F_STRING) ? al->data : "", al->fn, al->line);
		    al = al->abs_next;
		}
	    }
#endif
	}
	return;
    }

    fprintf(stderr, "------\nAllocation pool report %s (%s:%d):\n - allocated %ld bytes, %ld objects.\n - freed %ld bytes, %ld objects.\n - delta: %ld bytes, %ld objects\n------\n",
	    ap->region, fn, line,
	    (long) ap->allocs, (long) ap->n_alloc,
	    (long) ap->frees, (long) ap->n_free,
	    ((long) ap->allocs) - ((long) ap->frees),
	    ((long) ap->n_alloc) - ((long) ap->n_free));
    al = ap->root;
    while (al) {
	fprintf(stderr, "contains: %p (%ld) %s (allocated at %s:%d)\n",
		(void*) al->data, (long) al->size, (al->flags & F_STRING) ? al->data : "", al->fn, al->line);
	al = al->next;
    }
}

void  d_end_pool(allocation_pool *ap, const char *fn, int line) {
    allocation *al;
    if (!ap) {
	fprintf(stderr, "ERROR: d_end_pool(NULL)\n");
	return;
    }
#ifdef ALLOC_REPORT
    fprintf(stderr, "------\nAllocation pool report %s (%s:%d):\n - allocated %ld bytes, %ld objects.\n - freed %ld bytes, %ld objects.\n - delta: %ld bytes, %ld objects\n------\n",
	    ap->region, fn, line,
	    (long) ap->allocs, (long) ap->n_alloc,
	    (long) ap->frees, (long) ap->n_free,
	    ((long) ap->allocs) - ((long) ap->frees),
	    ((long) ap->n_alloc) - ((long) ap->n_free));
    if (ap == global)
	fprintf(stderr, "FATAL: attempt to release global pool\n");
#endif
    if (ap == last_pool)
	last_pool = ap->prev;
    if (ap->next) {
#ifdef ALLOC_REPORT
	fprintf(stderr, "WARNING: this pool is not the top of the stack: %s <%s> %s\n",
		ap->prev ? ap->prev->region : "NULL",
		ap->region,
		ap->next->region);
#endif
	ap->next->prev = ap->prev;
    }
    if (ap->prev)
	ap->prev->next = ap->next;
    al = ap->root;
    if (al) {
#ifdef ALLOC_REPORT
	if (last_pool)
	    fprintf(stderr, "NOTE: MOVING LEAKS from %s ==> %s\n", ap->region, last_pool->region);
#endif
	while (al) {
#ifdef ALLOC_REPORT
	    fprintf(stderr, "leaking: %p (%ld) %s (allocated at %s:%d)\n",
		    (void*) al->data, (long) al->size, (al->flags & F_STRING) ? al->data : "", al->fn, al->line);
#endif
	    if (last_pool) {
		allocation *me = al;
		last_pool->allocs += al->size;
		last_pool->n_alloc++;
		al->pool = last_pool;
		al = me->next;
		/* remove us from the original pool */
		if (me->prev)
		    me->prev->next = me->next;
		if (me->next)
		    me->next->prev = me->prev;
		/* add us to the last pool */
		me->prev = 0;
		me->next = last_pool->root;
		if (last_pool->root)
		    last_pool->root->prev = me;
		last_pool->root = me;
	    } else
		al = al->next;
	}
    }
}
