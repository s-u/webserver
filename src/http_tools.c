#include "http_tools.h"

#include <string.h>

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
            if (pos) { /* not a repeated / */
                *(dst++) = *(src++);
                pos = 0;
            } else /* repeated, just ignore */
                src++;
            continue;
        }
        pos++;
        if (*src >= 32)
            *(dst++) = *(src++);
    }
    *dst = 0;
}
