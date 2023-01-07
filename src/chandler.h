#include "http.h"

/* handlers return 0 if they choose to not handle the request.
   If a handler doesn't choose to handle the request it is not allowed
   to modify req/res.
   If path is present it is the full path to the resource.
*/
typedef int (*content_handler_fn_t)(http_request_t *req, http_result_t *res, void *user);

typedef struct content_handler content_handler_t;

/* add a new handler function and return the corresponding handler.
   If after is not NULL then the handler is inserted behind that handler, otherwise
   it is appended at the end.
   May return NULL if the handler cannot be allocated. */
content_handler_t *add_content_handler(content_handler_fn_t fn, void *user, content_handler_t *after);

/* call registered handlers sequentially. Returns the handler
   that took the request (if any) or NULL otherwise. */
content_handler_t *call_content_handlers(http_request_t *req, http_result_t *res);

/* remove one handler from the chain */
void remove_content_handler(content_handler_t *handler);

/* release all handlers */
void remove_allcontent_handlers(void);
