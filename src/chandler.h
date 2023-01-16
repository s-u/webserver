#include "http.h"

/* NOTE: content handlers are global and as such the adding,
   removal and listing of handlers is not thread-safe! */

/* handlers return 0 if they choose to not handle the request.
   If a handler doesn't choose to handle the request it is not allowed
   to modify req/res.
   If path is present it is the full path to the resource.
*/
typedef int (*content_handler_fn_t)(http_request_t *req, http_result_t *res, void *user);
typedef void (*free_handler_fn_t)(void *user);

typedef struct content_handler content_handler_t;

/* add a new handler function and return the corresponding handler.
   If after is not NULL then the handler is inserted behind that handler, otherwise
   it is appended at the end.
   May return NULL if the handler cannot be allocated. */
content_handler_t *add_content_handler(content_handler_fn_t fn, void *user, content_handler_t *after, free_handler_fn_t fh);

/* call registered handlers sequentially. Returns the handler
   that took the request (if any) or NULL otherwise. */
content_handler_t *call_content_handlers(http_request_t *req, http_result_t *res);

/* remove one handler from the chain */
void remove_content_handler(content_handler_t *handler);

/* list currently registered handlers.
   Always returns the number of currently registered handlers.
   if <buf> is not NULL, it will be filled with the handler pointers
   up to <count> of them. */
int list_content_handlers(content_handler_t **buf, int count);

/* release all handlers */
void free_content_handlers(void);
