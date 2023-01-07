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
http_static *http_add_static_entry(const char *prefix, const char *path,
				   const char *index, int flags);

/* removes and frees the entry, so the pointer may no longer be used */
void http_rm_static_entry(http_static *st);

/* handler to register with the http system to use this facility */
int http_static_handler(http_request_t *req, http_result_t *res, const char *path);

/* if the target is NULL then only returns the number of entries.
   max_ent is the number of entries that the buffer can hold.
   If there are more entries, they are not stored. */
int http_list_static_entries(http_static **st, int max_ent);

#endif
