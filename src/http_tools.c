#include "http_tools.h"

#include <string.h>

/* infer content type based on file name.
   The result is a static constant string. */
const char *infer_content_type(const char *fn) {
    const char *x = fn ? strrchr(fn, '.') : 0;
        char ext[8]; /* lower-case version of the extension */
        int i = 0;
    if (!x) return "application/octet-stream";
        x++;
        while (i < 7 && *x) {
                ext[i++] = (*x >= 'A' && *x <= 'Z') ? (*x | 0x20) : *x;
                x++;
        }
        ext[i] = 0;

    if (!strcmp(ext, "svg")) return "image/svg+xml";
    if (!strcmp(ext, "js"))  return "application/javascript";
    if (!strcmp(ext, "css")) return "text/css";
    if (!strcmp(ext, "html")) return "text/html";
    if (!strcmp(ext, "txt")) return "text/plain";
    if (!strcmp(ext, "png")) return "image/png";
    if (!strcmp(ext, "jpeg") || !strcmp(ext, "jpg")) return "image/jpeg";
    if (!strcmp(ext, "md")) return "text/markdown";
        if (!strcmp(ext, "json")) return "application/json";
    return "application/octet-stream";
}

/* returns a pointer to the beginning of a value for a given header
   field or NULL if not present. */
const char *get_header(http_request_t *req, const char *name) {
    const char *c = req->headers, *e;
    int name_len = strlen(name);
    if (!c) return 0;
    while (*c && (e = strchr(c, '\n'))) {
	const char *v = strchr(c, ':');
	if (v && (v < e) && (v - c == name_len)) {
	    int i;
	    for (i = 0; i < name_len; i++)
		if ((name[i] & 0xdf) != (c[i] & 0xdf))
		    break;
	    if (i == name_len) {
		v++;
		while (*v == '\t' || *v == ' ')
		    v++;
		return v;
	    }
	}
	while (*e == '\n' || *e == '\t') e++;
	c = e;
    }
    return 0;
}

/* removes .. and . segments, control characters in place.
   non-ASCII characters are retained */
void sanitize_path(char *path) {
    char *src = path, *dst = path;
    int pos = 0;
#ifdef WIN32
    /* Windows accepts \ as a separator and : introduces drives and
       NTFS streams, so neither may reach the file system as is */
    for (; *src; src++) {
        if (*src == '\\')
            *src = '/';
        if (*src != ':')
            *(dst++) = *src;
    }
    *dst = 0;
    src = dst = path;
#endif
    while (*src) {
        if (!pos) {
            if (src[0] == '.') {
                /* .. */
                if (src[1] == '.' && (!src[2] || src[2] == '/')) {
                    src += 2;
                    continue;
                }
                /* . (harmless, but we still remove it) */
                if (!src[1] || src[1] == '/') {
                    src++;
                    continue;
                }
            }
        }
        if (*src == '/') {
            if (dst > path && dst[-1] == '/') /* repeated / - ignore */
                src++;
            else
                *(dst++) = *(src++);
            pos = 0;
            continue;
        }
        pos++;
        /* char may be signed, bytes >= 0x80 (UTF-8) are retained;
           control characters are dropped, but must be skipped */
        if ((unsigned char) *src >= 32)
            *(dst++) = *src;
        src++;
    }
    *dst = 0;
}
