#include "chandler.h"

struct content_handler {
    struct content_handler *next, *prev;
    content_handler_fn_t process_request;
    free_handler_fn_t destroy;
    void *data;
};

static content_handler_t *handlers, *handlers_tail;

content_handler_t *add_content_handler(content_handler_fn_t fn, void *user, content_handler_t *after, free_handler_fn_t fh) {
    content_handler_t *h = calloc(sizeof(content_handler_t), 1);
    if (!h) return 0;
    h->next = 0;
    h->prev = handlers_tail;
    h->process_request = fn;
    h->data = user;
    h->destroy = fh;
    if (after) { /* insertion ? */
	h->prev = after;
	h->next = after->next;
	after->next = h;
	if (h->next)
	    h->next->prev = h;
	if (handlers_tail == after)
	    handlers_tail = h;
	return h;
    }
    if (!handlers)
	return (handlers = handlers_tail = h);
    handlers_tail->next = h;
    handlers_tail = h;
    return h;
}

void remove_content_handler(content_handler_t *handler) {
    if (handler) {
	if (handler->prev) {
	    handler->prev->next = handler->next;
	    if (handler->next)
		handler->next->prev = handler->prev;
	} else
	    handler->next->prev = 0;

	/* this could be moved up inside, since no prev means
	   we must be the root, but just in case ... */
	if (handlers == handler)
	    handlers = handler->next;
	if (handlers_tail == handler)
	    handlers_tail = handler->prev;
	if (handler->destroy)
	    handler->destroy(handler->data);
	free(handler);
    }
}

content_handler_t *call_content_handlers(http_request_t *req, http_result_t *res) {
    content_handler_t *h = handlers;
    while (h) {
        if (h->process_request(req, res, h->data))
	    return h;
        h = h->next;
    }
    return h;
}

void free_content_handlers(void) {
    while (handlers) {
        content_handler_t *n = handlers->next;
	if (handlers->destroy)
	    handlers->destroy(handlers->data);
        free(handlers);
        handlers = n;
    }
    handlers_tail = handlers;
}

int list_content_handlers(content_handler_t **buf, int count) {
    content_handler_t *h = handlers;
    int n = 0;
    while (h) {
	if (buf && n < count)
	    *(buf++) = h;
	n++;
	h = h->next;
    }
    return n;
}
