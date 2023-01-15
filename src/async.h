#ifndef ASYNC_H__
#define ASYNC_H__

#include "server.h"

typedef struct async_queue async_queue;

typedef struct async_job async_job;

async_queue *async_queue_new(void);
int  async_queue_add(async_queue *q, client_connection_t *conn);
/* this function may only be called from an already running async handler */
int  async_queue_add_inside(async_queue *q, client_connection_t *conn);
void async_queue_process(async_queue *q, double timeout);
void async_queue_stop(async_queue *q);

void async_signal_ready(async_queue *q, client_connection_t *conn, int lock);

/* compatible to be passed to async_run() */
void *async_job_process_queue(void *par);
/* there should be only one poll thread */
void *async_job_poll_queue(void *par);

async_job *async_queue_start_poll_thread(async_queue *q);

async_job *async_run(void * (thr)(void *), void *par);

#endif
