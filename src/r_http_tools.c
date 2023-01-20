#ifndef NO_R

#include "r_http_tools.h"

#ifndef NO_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#define MAX_HDR_NAME 256
#define MAX_ATTR_NAME 256

/* initial size of an inline buffer for header parsing - it
   is used to store the content of one header attribute.
   Larger attribute content is allowed, but will result in R_alloc()
   and thus GC pressure, but header attributes of 1k+ should be rare. */
#define INLINE_HEADER_BUFFER 1024

SEXP r_http_parse_headers(const char *hdrs, int len_spec) {
    if (!hdrs)
	return R_NilValue;
    char buf[INLINE_HEADER_BUFFER];
    size_t len = (size_t) (len_spec < 0 ? strlen(hdrs) : len_spec);
    const char *c = hdrs, *e = c + len;
    SEXP res = PROTECT(CONS(R_NilValue, R_NilValue)), tail = res;
    while (c < e) {
	const char *bol = c;
	while (c < e && *c != ':' && *c != '\r' && *c != '\n')
	    c++;
	if (c < e && *c == ':') {
	    char *dst = buf;
	    SEXP htail = 0;
	    if (c - bol > MAX_HDR_NAME)
		break;
	    /* turn header entries into lowercase and - into . */
	    while (bol < c) {
		*(dst++) = (*bol >= 'A' && *bol <= 'Z') ? (*bol | 0x20) :
		    ( (*bol == '-') ? '.' : *bol );
		bol++;
	    }
	    *dst = 0;
	    SET_TAG(tail, Rf_install(buf));
	    /* end of header name, parse attributes (if any) */
	    c++; /* skip : */
	    while (c < e) {
		while (c < e && (*c == ' ' || *c == '\t')) c++;
		/* EOL ? */
		if ((c < e && c[0] == '\n') || (c + 1 < e && c[0] == '\r' && c[1] == '\n')) {
		    c += (*c == '\n') ? 1 : 2;
		    /* next line - if not continuation, bail out */
		    if (c < e && *c != ' ' && *c != '\t')
			break;
		    /* if it is, back to removing whitespace */
		    /* FIXME: are continuations implying extra whitespace? */
		    continue;
		}

		/* next attribute */
		SEXP sVal = 0;
		char *avloc = 0, *asep = 0;
		size_t alen = 0;
		const char *av = c;
		/* PASS 1: find out the attr size */
		while (c < e && *c != '\r' && *c != '\n' && *c != ';') {
		    if (*c == '"') { /* process quoted parts */
			c++;
			while (c < e && *c != '"') {
			    if (*c == '\\' && c + 1 < e)
				c++;
			    c++;
			    alen++;
			}
		    } else if (*c == ';')
			break;
		    else
			alen++;
		    c++;
		}
		if (!alen)
		    break;

		/* allocate buffer (if necessary) */
		if (alen >= sizeof(buf)) {
		    avloc = R_alloc(alen + 1, 1);
		    dst = avloc;
		} else
		    dst = buf;
		c = av;

		/* PASS 2: copy as you go */
		while (c < e && *c != '\r' && *c != '\n' && *c != ';') {
		    if (*c == '"') { /* process quoted parts */
			c++;
			while (c < e && *c != '"') {
			    if (*c == '\\' && c + 1 < e)
				c++;
			    *(dst++) = *(c++);
			}
			if (c < e)
			    c++;
		    } else if (*c == ';')
			break;
		    else {
			if (*c == '=' && !asep)
			    asep = dst;
			*(dst++) = *(c++);
		    }
		}
		*dst = 0;
		if (asep)
		    *asep = 0;
		if (asep) {
		    SEXP sAStr = Rf_mkString(asep + 1);
		    sVal = PROTECT(CONS(sAStr, R_NilValue));
		    SET_TAG(sVal, Rf_install(avloc ? avloc : buf));
		} else {
		    sVal = PROTECT(CONS(Rf_mkString(avloc ? avloc : buf), R_NilValue));
		}
		if (!htail) {
		    htail = sVal;
		    SETCAR(tail, htail);
		} else {
		    SETCDR(htail, sVal);
		    htail = sVal;
		}
		UNPROTECT(1);
		if (c < e && *c == ';')
		    c++;
	    }
	    /* done with this header */
	    SEXP newTail = CONS(R_NilValue, R_NilValue);
	    SETCDR(tail, newTail);
	    tail = newTail;
	} else { /* line without : = garbage, bail out */
	    break;
	}
    }
    tail = res;
    if (CAR(res) == R_NilValue)
	res = R_NilValue;
    else { /* remove the last, empty CONS */
	while (CAR(CDR(tail)) != R_NilValue)
	    tail = CDR(tail);
	SETCDR(tail, R_NilValue);
    }
    UNPROTECT(1);
    return res;
}

SEXP r_http_parse_query(char *query)
{
    int parts = 0;
    SEXP res, names;
    char *s = query, *key = 0, *value = query, *t = query;
    if (!query)
	return allocVector(STRSXP, 0);
    while (*s) {
	if (*s == '&') parts++;
	s++;
    }
    parts++;
    res = PROTECT(allocVector(STRSXP, parts));
    names = PROTECT(allocVector(STRSXP, parts));
    s = query;
    parts = 0;
    while (1) {
	if (*s == '=' && !key) { /* first '=' in a part */
	    key = value;
	    *(t++) = 0;
	    value = t;
	    s++;
	} else if (*s == '&' || !*s) { /* next part */
	    int last_entry = !*s;
	    *(t++) = 0;
	    if (!key) key = "";
	    SET_STRING_ELT(names, parts, mkChar(key));
	    SET_STRING_ELT(res, parts, mkChar(value));
	    parts++;
	    if (last_entry) break;
	    key = 0;
	    value = t;
	    s++;
	} else if (*s == '+') { /* + -> SPC */
	    *(t++) = ' '; s++;
	} else if (*s == '%') { /* we cannot use uri_decode becasue we need &/= *before* decoding */
	    unsigned char ec = 0;
	    s++;
	    if (*s >= '0' && *s <= '9') ec |= ((unsigned char)(*s - '0')) << 4;
	    else if (*s >= 'a' && *s <= 'f') ec |= ((unsigned char)(*s - 'a' + 10)) << 4;
	    else if (*s >= 'A' && *s <= 'F') ec |= ((unsigned char)(*s - 'A' + 10)) << 4;
	    if (*s) s++;
	    if (*s >= '0' && *s <= '9') ec |= (unsigned char)(*s - '0');
	    else if (*s >= 'a' && *s <= 'f') ec |= (unsigned char)(*s - 'a' + 10);
	    else if (*s >= 'A' && *s <= 'F') ec |= (unsigned char)(*s - 'A' + 10);
	    if (*s) s++;
	    *(t++) = (char) ec;
	} else *(t++) = *(s++);
    }
    setAttrib(res, R_NamesSymbol, names);
    UNPROTECT(2);
    return res;
}

#endif
