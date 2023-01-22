#include "async.h"
#include "ulog.h"

#ifdef WIN32
#include "win32.h"
#else
#include <poll.h>
#endif
#include <pthread.h>

#include <string.h>

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

struct async_job {
    pthread_t thread;
    pthread_attr_t thread_attr;
};

typedef struct async_job_list {
    size_t n, alloc;
    async_job *job[1];
} async_job_list;

struct async_queue {
    async_conn *head, *tail;
    pthread_mutex_t mux;
    pthread_cond_t  cond;
    async_job *poll_job;
    async_job_list *process_jobs;

    nfds_t pf_alloc;
    int n;
    SOCKET pfd[2];
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
    q->poll_job = 0;
    q->process_jobs = 0;
    pipe(q->pfd);
    q->active = 1;
    pthread_mutex_init(&(q->mux), 0);
    pthread_cond_init(&(q->cond), 0);
    DBG(ulog_info("async_queue_new: create new queue %p", (void*) q));
    return q;
}

static async_job_list *async_job_list_ensure(async_job_list *orig, int alloc) {
    async_job_list *njl = 0;
    int real_alloc = orig ? (alloc * 2) : (alloc < 16 ? 16 : alloc);
    /* if orig has the space, nothing to do */
    if (orig && orig->alloc >= alloc)
	return orig;
    njl = (async_job_list*) malloc(sizeof(async_job_list) + real_alloc * sizeof(async_job*));
    if (!njl)
	return 0;
    if (orig) { /* copy all existing entries */
	memcpy(njl, orig, sizeof(async_job_list) + orig->alloc * sizeof(async_job*));
	free(orig);
    } else
	njl->n = 0;
    njl->alloc = real_alloc;
    return njl;
}

int  async_queue_signal_ready(async_queue *q, client_connection_t *conn) {
    async_conn *c;
    if (!q || !conn)
	return -1;
    pthread_mutex_lock(&(q->mux));
    c = q->head;
    while (c && c->conn != conn)
	c = c->next;
    if (!c) {
	pthread_mutex_unlock(&(q->mux));
	return -1;
    }
    c->state |= ACS_READY;

    pthread_cond_broadcast(&(q->cond));
    pthread_mutex_unlock(&(q->mux));
    return 0;
}

void async_queue_begin(async_queue *q) {
    if (q)
	pthread_mutex_lock(&(q->mux));
}

void async_queue_end(async_queue *q) {
    if (q)
	pthread_mutex_unlock(&(q->mux));
}

static int async_queue_add_(async_queue *q, client_connection_t *conn, int lock) {
    async_conn *ac;
    if (!conn)
	return 0;
    if (!q || !q->active || !(ac = calloc(sizeof(async_conn), 1)))
	return -1;
    DBG(ulog_info("async_queue_add: adding connection %p (%d)", (void*) conn, conn->info.s));
    ac->conn = conn;
    ac->state = 0;
    if (lock) pthread_mutex_lock(&(q->mux));

    /* in the unlikely case of too many connections we re-allocate the poll list */
    if (q->n >= q->pf_alloc) {
	size_t nalloc = q->pf_alloc * 2;
	struct pollfd *npf = (struct pollfd *) malloc(sizeof(struct pollfd) * nalloc);
	if (!npf) {
	    if (lock) pthread_mutex_unlock(&(q->mux));
	    return -1;
	}
	if (q->pf != q->pf_)
	    free(q->pf);
	q->pf = npf;
    }

    if (q->active) {
	if (!q->head)
	    q->head = q->tail = ac;
	else {
	    ac->prev = q->tail;
	    q->tail->next = ac;
	    q->tail = ac;
	}
	q->n++;

#ifdef WIN32
	send(q->pfd[1], "", 1, 0);
#else
	write(q->pfd[1], "", 1);
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
	} else if (c->conn->info.s != FD_VIRTUAL) {
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

    if (r > 0) {
	i = 0;
	pthread_mutex_lock(&(q->mux));
	/* the first entry in the fds is always the wake-up pipe */
	if (q->pf[i].revents) {
	    char xx[4];
#ifdef WIN32
	    recv(q->pf[i].fd, xx, 4, 0);
#else
	    read(q->pf[i].fd, xx, 4);
#endif
	}
	i++;
	/* we rely on the fact that no one can remove from the queue, so the poll list
	   must be always the first n connections - that way we don't have to keep
	   another list. We flag all ready entries and then signal other threads to
	   process. */
	async_conn *c = q->head;
	while (c && i < n) {
	    if (!(c->state & ACS_CLOSED)) {
		/* we want to signal ready only on connections that are not processing,
		   because processing can read from the connection and thus the ready
		   state may become invalid (in fact that is most commonly the case
		   since that is the point of the processing).
		   NB that during processing the poll will be spinning until the
		   processing has cleared the ready state for the socket. However,
		   we can't really remove processing sockets or else the poll()
		   would not be woken up by new data on sockets that are being processed.
		   Put differently, we have no way to distinguish the ready part that
		   is expected to be pulled by the process vs which will remain for the
		   next processing iteration. In practice most handlers will read the data
		   first, clearing the ready state anyway. If the CPU usage by such
		   spinning polls is really worrying us then we could have the processing
		   signal on the wake-up pipe - but then we may as well just use libevent
		   instead if that's what we want to worry about. */
		if (q->pf[i].revents && !(c->state & ACS_PROCESSING)) {
		    DBG(ulog_debug("async_queue_poll(%p): connection %p (%d = %d) is ready",
				   (void*) q, (void*) c->conn, c->conn->info.s, q->pf[i].fd));
		    c->state |= ACS_READY;
		} else
		    c->state &= ~ACS_READY;
		i++;
	    }
	    c = c->next;
	}
	pthread_cond_broadcast(&(q->cond));
	pthread_mutex_unlock(&(q->mux));
    }
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
#ifdef __APPLE__
	    /* on macOS we need a run-time check since R binaires support 10.13 and up */
	    if (__builtin_available(macOS 10.15, *)) {
		timespec_get(&ts, TIME_UTC);
	    } else {
		clock_gettime(CLOCK_REALTIME, &ts);
	    }
#else
	    timespec_get(&ts, TIME_UTC);
#endif
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

void async_set_name(const char *name) {
#ifdef __APPLE__
    pthread_setname_np(name);
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

void *async_job_process_queue(void *par) {
    async_queue *q = (async_queue*) par;
    char name[16] = "aq_proc_000";
    pthread_mutex_lock(&(q->mux));
    if (q->process_jobs) { /* this is mostly vain, but we find our place in the list */
	int i = 0, n = q->process_jobs->n;
	pthread_t tself = pthread_self();
	while (i < n) {
	    if (q->process_jobs->job[i] && q->process_jobs->job[i]->thread == tself)
		break;
	    i++;
	}
	if (i >= n)
	    name[7] = 0;
	else {
	    name[10] = (i % 10) + '0';
	    name[9] = ((i / 10) % 10) + '0';
	    name[8] = ((i / 100) % 10) + '0';
	}
    }
    pthread_mutex_unlock(&(q->mux));
    async_set_name(name);
    DBG(ulog_debug("async_job_process_queue(%p): starting loop in thread %s", par, name));
    while (q && q->active) {
	async_queue_process(q, 60.0);
    }
    DBG(ulog_debug("async_job_process_queue(%p): ending loop for thread %s", par, name));
    return par;
}

void *async_job_poll_queue(void *par) {
    async_queue *q = (async_queue*) par;
    DBG(ulog_debug("async_job_poll_queue(%p): starting loop", par));
    async_set_name("poll_queue");
    while (q && q->active) {
	async_queue_poll(q, 20.0);
    }
    DBG(ulog_debug("async_job_poll_queue(%p): ending loop", par));   
    return par;
}

void async_queue_stop(async_queue *q) {
    if (q)
	q->active = 0;
}

int async_queue_finish(async_queue *q) {
    void *ret = 0;
    if (!q)
	return -1;
    /* first, flag the queue as inactive to prevent further
       thread spawning */
    pthread_mutex_lock(&(q->mux));
    q->active = 0;
    pthread_mutex_unlock(&(q->mux));

    /* trigger poll */
#ifdef WIN32
    send(q->pfd[1], "", 1, 0);
#else
    write(q->pfd[1], "", 1);
#endif
    if (q->poll_job) {
	DBG(ulog_info("async_queue_finish: waiting for poll thread"));
	if (pthread_join(q->poll_job->thread, &ret))
	    ulog_error("cannot join poll thread");
    }
    /* before we can collect process threads we have to
       close any (non-processing?) connections or else
       the clients can have us stuck indefinitely */
    pthread_mutex_lock(&(q->mux));
    if (q->head || q->tail) {
	DBG(ulog_debug("async_queue_finish: initiating shutdown on connections"));
	async_conn *c = q->head;
	while (c) {
	    DBG(ulog_debug(" - connection %p (%d), state=%d, shutdown", (void*) c->conn, c->conn->info.s, c->state));
	    if (c->conn->info.s != INVALID_SOCKET && c->conn->info.s != FD_VIRTUAL)
		shutdown(c->conn->info.s, SHUT_RD);
	    /* FIXME: should we tell the connection so it can be more explicit about handling this? */
	    c = c->next;
	}
    }
    pthread_mutex_unlock(&(q->mux));

    if (q->process_jobs) {
	int i = 0, n = q->process_jobs->n;
	while (i < n) {
	    DBG(ulog_info("async_queue_finish: waiting for process thread %d", i));
	    if (pthread_join(q->process_jobs->job[i]->thread, &ret))
		ulog_error("cannot join process thread %d", i);
	    i++;
	}
    }
    /* everyone joined */
    pthread_mutex_lock(&(q->mux));
    DBG(ulog_info("async_queue_finish: freeing all resources"));
    if (q->poll_job)
	free(q->poll_job);
    if (q->process_jobs) {
	int i = 0, n = q->process_jobs->n;
	while (i < n)
	    free(q->process_jobs->job[i++]);
	free(q->process_jobs);
	q->process_jobs = 0;
    }
    /* check for any pending connections */
    if (q->head || q->tail) {
	DBG(ulog_debug("async_queue_finish: releasing pending connections"));
	async_conn *c = q->head;
	while (c) {
	    DBG(ulog_debug(" - connection %p (%d), state=%d", (void*) c->conn, c->conn->info.s, c->state));
	    async_conn *me = c;
	    c = c->next;
	    /* fin + free the underlying connection */
	    if (me->conn->info.srv->conn_fin)
		me->conn->info.srv->conn_fin(me->conn);
	    free(me->conn);
	    free(me);
	}
    }
    pthread_mutex_unlock(&(q->mux));
    return 0;
}

void async_queue_free(async_queue *q) {
    if (q) {
	if (q->pf != q->pf_)
	    free(q->pf);
	pthread_mutex_destroy(&(q->mux));
	pthread_cond_destroy(&(q->cond));
	free(q);
    }
}

async_job *async_run(void * (thr)(void *), void *par) {
    async_job *job = (async_job*) malloc(sizeof(async_job));
    if (!job)
	return job;

    pthread_attr_init(&job->thread_attr);
    /* pthread_attr_setdetachstate(&job->thread_attr, PTHREAD_CREATE_DETACHED); */
    if (pthread_create(&job->thread, &job->thread_attr, *thr, par)) {
	free(job);
	job = 0;
    }
    DBG(ulog_debug("async_run: created new thread %p", (void*) job));
    return job;
}
 
async_job *async_queue_start_poll_thread(async_queue *q) {
    pthread_mutex_lock(&(q->mux));
    /* FIXME: we refuse to start a second thread.
       This is likely a non-issue, but in theory if one wanted
       to re-start it due to some failure this could be of interest. */
    if (q->poll_job || !q->active)
	return q->poll_job;
    async_job *j = async_run(async_job_poll_queue, q);
    q->poll_job = j;
    pthread_mutex_unlock(&(q->mux));
    return j;
}

int async_queue_set_process_threads(async_queue *q, int n) {
    int res = -1;
    if (!q || !q->active)
	return -1;
    pthread_mutex_lock(&(q->mux));
    if (q->process_jobs && q->process_jobs->n >= n) {
	pthread_mutex_unlock(&(q->mux));
	return q->process_jobs->n;
    }
    async_job_list *l = async_job_list_ensure(q->process_jobs, n);
    if (!l) {
	pthread_mutex_unlock(&(q->mux));
	return -1;
    }
    q->process_jobs = l;
    while (q->process_jobs->n < n) {
	async_job *j = async_run(async_job_process_queue, q);
	if (!j) break;
	q->process_jobs->job[q->process_jobs->n++] = j;
    }
    res = q->process_jobs->n;
    pthread_mutex_unlock(&(q->mux));
    return res;
}
