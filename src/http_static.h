#ifndef HTTP_STATIC_H__
#define HTTP_STATIC_H__

#include "http.h"

#define HSF_STOP          1 /* stop if prefix matches */
#define HSF_PRECOMPRESSED 2 /* use pre-compressed .gz files */

typedef struct http_static http_static;

/* prefix - path with that prefix will be served. Uses byte-wise prefix
            comparison
   path   - local file system path to prepend before the content following
            the prefix to contruct a file path.
   index  - if not NULL then if a directory is matched then this will
            be appended to find a file (should contain leading /).
   flags  - see constants above

   returns NULL on error or an object representing that entry on success.
*/
http_static *http_static_entry(const char *prefix, const char *path,
			       const char *index, int flags);
void http_static_free(http_static *hs);

/* handler to register with the http system to use this facility */
int http_static_handler(http_request_t *req, http_result_t *res, void *user);

/* typical use:
   add_content_handler(http_static_handler,
                       http_static_entry("/", "./", "/index.html", 0),
		       NULL);
*/

#endif
