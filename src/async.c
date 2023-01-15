#include "async.h"
#include "ulog.h"

#include <poll.h>
#include <pthread.h>

#include <stdio.h>

struct client_connection {
    /* common information for all connections */
    client_connection_info_t info;
};

#define ACS_READY      1
#define ACS_PROCESSING 2
#define ACS_CLOSED     4

typedef struct async_conn {
    struct async_conn *next, *prev;
    client_connection_t *conn;
    int state;
} async_conn;

struct async_queue {
    async_conn *head, *tail;
    pthread_mutex_t mux;
    pthread_cond_t  cond;
    pthread_t poll_thread;
    nfds_t pf_alloc;
    int n;
    int pfd[2];
    volatile int active;
    struct pollfd *pf;
    struct pollfd pf_[1];
};

async_queue *async_queue_new(void) {
    nfds_t max_pf = 8192;
    async_queue *q = (async_queue*) malloc(sizeof(async_queue) + sizeof(struct pollfd) * max_pf);
    if (!q)
	return q;

    q->head = q->tail = 0;
    q->pf_alloc = max_pf;
    q->pf = q->pf_;
    q->n = 0;
    pipe(q->pfd);
    q->active = 1;
    pthread_mutex_init(&(q->mux), 0);
    pthread_cond_init(&(q->cond), 0);
    DBG(ulog_info("async_queue_new: create new queue %p", (void*) q));
    return q;
}

static int async_queue_add_(async_queue *q, client_connection_t *conn, int lock) {
    async_conn *ac;
    if (!conn)
	return 0;
    if (!q || !(ac = calloc(sizeof(async_conn), 1)))
	return -1;
    DBG(ulog_info("async_queue_add: adding connection %p (%d)", (void*) conn, conn->info.s));
    ac->conn = conn;
    ac->state = 0;
    if (lock) pthread_mutex_lock(&(q->mux));
    if (!q->head)
	q->head = q->tail = ac;
    else {
	ac->prev = q->tail;
	q->tail->next = ac;
	q->tail = ac;
    }
    q->n++;
    {
	/* FIXME: the polling thread is signaling workers which
	   connections are ready to go, but if there is a new
	   connection, the poll() call will not know about it
	   and time out. */
#if 0
	struct pollfd pf;
	int pr;
	pf.fd = conn->info.s;
	pf.events = POLLIN;
	//if ((pr = poll(&pf, 1, 0)) > 0)
	pr = poll(&pf, 1, 0);
	ac->state |= ACS_READY;
	ulog_error("poll on new connection (%d) returned %d", pf.fd, pr);
	pthread_cond_signal(&(q->cond));
#else
	//fprintf(stderr, "signal new connection\n");
	//fprintf(stderr, "write = %d\n",
	write(q->pfd[1], "", 1);
	//	if (q->poll_thread)
	//  pthread_kill(q->poll_thread, SIGUSR1);
#endif
    }

    if (lock) pthread_mutex_unlock(&(q->mux));
    return 0;
}

int async_queue_add(async_queue *q, client_connection_t *conn) {
    return async_queue_add_(q, conn, 1);
}

int async_queue_add_inside(async_queue *q, client_connection_t *conn) {
    return async_queue_add_(q, conn, 0);
}

#include <stdio.h>

static int async_queue_setup_poll(async_queue *q, int rm) {
    int n = 0;
    async_conn *c = q->head;
    q->pf[n].fd = q->pfd[0];
    q->pf[n].events = POLLIN;
    n++;
    while (c && n <= q->n) {
	if (c->conn->info.s == -1) { /* closed */
	    c->state |= ACS_CLOSED; /* we flag it as closed so we can skip it in our list */
	    if (rm && !(c->state & ACS_PROCESSING)) { /* remove if it's not processing */
		async_conn *me = c;
		if (c->conn->info.srv->conn_fin)
		    c->conn->info.srv->conn_fin(c->conn);
		free(c->conn);
		c->conn = 0;
		if (c->prev)
		    c->prev->next = c->next;
		if (c->next)
		    c->next->prev = c->prev;
		if (q->head == c)
		    q->head = c->next;
		if (q->tail == c)
		    q->tail = c->prev;
		c = c->next;
		free(me);
		continue;
	    }
	} else {
	    q->pf[n].fd = c->conn->info.s;
	    q->pf[n].events = POLLIN;
	    n++;
	}
	c = c->next;
    }
    return n;
}

/* remove connections with invalid sockets from the queue and then
   checks readiness of all of them, flagging those that are ready
   with ACS_READY and signals the condition if any are ready. */
void async_queue_poll(async_queue *q, double timeout) {
    int i = 0, n = 0, r;

    DBG(ulog_debug("async_queue_process(%p): obtaining mutex", (void*) q));
    pthread_mutex_lock(&(q->mux));
    DBG(ulog_debug(" - got mutex (%p)", (void*) q));
    /* collect all valid connections, remove closed ones */
    n = async_queue_setup_poll(q, 1);
    pthread_mutex_unlock(&(q->mux));

    DBG(ulog_debug("async_queue_poll(%p): start poll(%d, %f)", (void*) q, n, timeout));
    r = poll(q->pf, n, (int)(timeout * 1000.0));
    DBG(ulog_debug("async_queue_poll(%p): poll(%d, %f) -> %d", (void*) q, n, timeout, r));

#if 0
    {
	fprintf(stderr, "async_queue_poll(%p): poll(%d, %f) -> %d  ", (void*) q, n, timeout, r);
	int ii = 0;
	while (ii < n) { fprintf(stderr, "%c", (q->pf[ii].revents) ? 'X' : '.'); ii++; }
	fprintf(stderr, "\n");
    }
#endif
    if (r > 0) {
	i = 0;
	/* we rely on the fact that no one can remove from the queue, so the poll list
	   must be always the first n connections - that way we don't have to keep
	   another list. We flag all ready entries and then signal other threads to
	   process. */
	pthread_mutex_lock(&(q->mux));
	if (q->pf[i].revents) {
	    char xx[4];
	    read(q->pf[i].fd, xx, 4);
	}
	i++;
	async_conn *c = q->head;
	while (c && i < n) {
	    if (!(c->state & ACS_CLOSED)) {
		if (q->pf[i].revents)
		    c->state |= ACS_READY;
		else
		    c->state &= ~ACS_READY;
		i++;
	    }
	    c = c->next;
	}
	pthread_cond_broadcast(&(q->cond));
	pthread_mutex_unlock(&(q->mux));
    }
}

void async_signal_ready(async_queue *q, client_connection_t *conn, int lock) {
    if (!q)
	return;

    if (lock)
	pthread_mutex_lock(&(q->mux));
    async_conn *c = q->head;
    while (c && c->conn != conn)
	c = c->next;
    if (c && c->conn->info.s != -1) {
	struct pollfd pf;
	pf.fd = c->conn->info.s;
	pf.events = POLLIN;
	if (poll(&pf, 1, 0) > 0)
	    c->state |= ACS_READY;
	pthread_cond_signal(&(q->cond));
    }
    if (lock)
	pthread_mutex_unlock(&(q->mux));
}

/* picks the first ready connecton and calls process() on it.
   if none are ready, waits up to timeout for one to be ready */
void async_queue_process(async_queue *q, double timeout) {
    int tries = 0;
    if (!q)
	return;
    /* only do one try without waiting, then one with waiting 
       and if that still doesn't yield processed connection,
       we give up */
    while (tries < 2) {
	async_conn *c;
	pthread_mutex_lock(&(q->mux));
	if (tries++) { /* wait only if we checked already once */
	    struct timespec ts;
	    /* possibilities: gettimeofday, clock_gettime, timespec_get */
	    timespec_get(&ts, TIME_UTC);
	    ts.tv_nsec += (timeout - (double)((int) timeout)) * 1000000000.0;
	    ts.tv_sec += (int) timeout;
	    if (ts.tv_nsec >= 1000000000.0) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000.0;
	    }
	    DBG(ulog_debug("async_queue_process waiting for condition"));
	    if (pthread_cond_timedwait(&(q->cond), &(q->mux), &ts)) {
		DBG(ulog_debug("async_queue_process timed out"));
		/* timeout (or other error for that matter) */
		pthread_mutex_unlock(&(q->mux));
		return;
	    }
	}
	/* check if any connection is ready */
	c = q->head;
	while (c) {
	    /* check for a connection that is valid, ready and not processing */
	    if ((c->state & (ACS_READY | ACS_PROCESSING)) == ACS_READY &&
		c->conn->info.s != -1) {
		c->state |= ACS_PROCESSING;
		/* unlock for the processing stage */
		pthread_mutex_unlock(&(q->mux));
		DBG(ulog_debug(" - async_queue_process calling process() on %p (%d)", (void*) c->conn, c->conn->info.s));
		c->conn->info.srv->process(c->conn);
		DBG(ulog_debug(" - async_queue_process process() returned on %p (%d)", (void*) c->conn, c->conn->info.s));
		/* lock to signal we're done */
		pthread_mutex_lock(&(q->mux));
		c->state &= ~ACS_PROCESSING;
		pthread_mutex_unlock(&(q->mux));
		return; /* done */
	    }
	    c = c->next;
	}
	pthread_mutex_unlock(&(q->mux));
    }
}

void *async_job_process_queue(void *par) {
    async_queue *q = (async_queue*) par;
    DBG(ulog_debug("async_job_process_queue(%p): starting loop", par));
    while (q && q->active) {
	async_queue_process(q, 1.0);
    }
    DBG(ulog_debug("async_job_process_queue(%p): ending loop", par));   
    return par;
}

void *async_job_poll_queue(void *par) {
    async_queue *q = (async_queue*) par;
    DBG(ulog_debug("async_job_poll_queue(%p): starting loop", par));
    while (q && q->active) {
	async_queue_poll(q, 1.0);
    }
    DBG(ulog_debug("async_job_poll_queue(%p): ending loop", par));   
    return par;
}

void async_queue_stop(async_queue *q) {
    if (q)
	q->active = 0;
}

struct async_job {
    pthread_t thread;
    pthread_attr_t thread_attr;
};

async_job *async_run(void * (thr)(void *), void *par) {
    async_job *job = (async_job*) malloc(sizeof(async_job));
    if (!job)
	return job;

    pthread_attr_init(&job->thread_attr);
    pthread_attr_setdetachstate(&job->thread_attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&job->thread, &job->thread_attr, *thr, par)) {
	free(job);
	job = 0;
    }
    DBG(ulog_debug("async_run: created new thread %p", (void*) job));
    return job;
}
 
async_job *async_queue_start_poll_thread(async_queue *q) {
     async_job *j = async_run(async_job_poll_queue, q);
     q->poll_thread = j->thread;
     return j;
 }
