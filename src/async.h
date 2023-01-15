#ifndef ASYNC_H__
#define ASYNC_H__

#include "server.h"

typedef struct async_queue async_queue;

typedef struct async_job async_job;

/* this special value for fd in a connection can be used when
   there is no underlying fd and it still should be considered valid. */
#define FD_VIRTUAL -2

async_queue *async_queue_new(void);
int  async_queue_add(async_queue *q, client_connection_t *conn);
/* this function may only be called from an already running async handler */
int  async_queue_add_inside(async_queue *q, client_connection_t *conn);
void async_queue_process(async_queue *q, double timeout);
void async_queue_stop(async_queue *q);

/* signal readiness of a connection if no poll thread is used */
int  async_queue_signal_ready(async_queue *q, client_connection_t *conn);

/* joins all processing threads (finsihing any processing that is still
   in flight) and releases all connections */
int async_queue_finish(async_queue *q);
/* this should be called only if async_queue_finish terminated without an error. */
void async_queue_free(async_queue *q);

/* compatible to be passed to async_run() */
void *async_job_process_queue(void *par);
/* there should be only one poll thread */
void *async_job_poll_queue(void *par);

async_job *async_run(void * (thr)(void *), void *par);

/* use the following two to manage poll and process threads for a queue */
async_job *async_queue_start_poll_thread(async_queue *q);
/* this ensures at least n processing theads are running and returns
   the actual number of threads */
int async_queue_set_process_threads(async_queue *q, int n);

/* enter and leave critical region. You MUST guarantee that
   the calls are always matched and they MUST NOT nest.
   Experimental, for expert use only, use with care. */
void async_queue_begin(async_queue *q);
void async_queue_end(async_queue *q);

#endif
