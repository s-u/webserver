#ifndef HTTP_TOOLS_H__
#define HTTP_TOOLS_H__

#include "http.h"

const char *get_header(http_request_t *req, const char *name);
void sanitize_path(char *path);

#endif
