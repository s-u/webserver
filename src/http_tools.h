#ifndef HTTP_TOOLS_H__
#define HTTP_TOOLS_H__

#include "http.h"

const char *get_header(http_request_t *req, const char *name);
void sanitize_path(char *path);

/* Infer content type based on file name.
   The result is a static constant string.
   Defaults to "application/octet-stream" if not detected.
*/
const char *infer_content_type(const char *fn);

#endif
