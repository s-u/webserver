/* TCP/UDP logger compatible with syslog

   (C)Copyright 2002-2022 Simon Urbanek

   This logger can be used either via simple

   ulog(UL_INFO, ...) or
   ulog_info(...)

   call in the printf format, or the message can be constructed
   incerementally via

   ulog_begin(UL_INFO)
   ulog_add(...); [ulog_add(...); ...]
   ulog_end()

   calls. ulog_end() commences the transfer.
   Internal state, sockets etc. may be cached after the first use.

   The levels (aka severity) are defined in ulog.h and correspond
   to 0 .. 7 of syslog, typically UL_INFO, UL_DEBUG, UL_WARN and
   UL_ERROR. See ulog.h for the full list and convenience macros
   such as ulog_info(), ulog_error(), etc.

*/

#define DEFAULT_ULOG_PORT 514

#ifndef NO_CONFIG_H
#include "config.h"
#endif

#include "ulog.h"

const char *prefix[8] = { "EMERGENCY", "ALERT", "CRITICAL", "ERROR",
			  "WARNING", "NOTE", "INFO", "DEBUG" };
int prefix_len[8] = { 9, 5, 8, 5, 7, 4, 4, 5 };

/* FIXME: now that we support UDP/TCP we could make this work on Windows ... */
#ifndef WIN32

#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#if HAVE_SYS_TIME_H
# include <sys/time.h>
#endif
#include <time.h>
#include <stdio.h>
#include <stdarg.h>

#ifndef AF_LOCAL
#define AF_LOCAL AF_UNIX
#endif

static int   ulog_sock = -1;
static char *ulog_path;

#ifndef NO_ULOG
static int   ulog_dcol, ulog_port = 0;

static char hn[512];
static __thread char buf[4096];
static __thread char ts[64];
static __thread unsigned int buf_pos, buf_preamble, buf_level;
#ifdef ULOG_MICROTIME
static __thread double time0, timeN;
#endif
#endif
/* RFC5424 limits app to 48 chars */
static char app_name[48] = "ulog-unknown";
static int facility = 3; /* sys daemon */

static char *sstrdup(const char *s, const char *dflt) {
    char *c;
    if (!s) return 0;
    c = strdup(s);
    return (char*) (c ? c : dflt);
}

void ulog_set_app_name(const char *name) {
    if (strlen(name) > 47) {
	memcpy(app_name, name, 47);
	app_name[47] = 0;
    } else
	strcpy(app_name, name);
}

void ulog_set_path(const char *path) {
    ulog_path = sstrdup(path, 0);
}

void ulog_set_facility(int fac) {
    if (fac < 0 || fac > 23)
	fac = 23;
    facility = fac;
}

int ulog_enabled(void) {
    return (ulog_path) ? 1 : 0;
}

void ulog_reset(void) {
    if (ulog_sock != -1)
	close(ulog_sock);
    ulog_sock = -1;
}

#ifndef NO_ULOG
void ulog_begin(int level) {
    if (!ulog_path) return;

    if (ulog_sock == -1) { /* first-time user */
	int u_family = AF_LOCAL;
	int u_sock   = SOCK_DGRAM;
	gethostname(hn, sizeof(hn));
	if (!strncmp(ulog_path, "udp://", 6) || !strncmp(ulog_path, "tcp://", 6)) {
	    const char *c;
	    u_family = AF_INET;
	    if (ulog_path[0] == 't') u_sock = SOCK_STREAM;
	    c = strchr(ulog_path + 6, ':');
	    ulog_port = DEFAULT_ULOG_PORT;
	    if (c) {
		ulog_dcol = (int) (c - ulog_path);
		ulog_port = atoi(c + 1);
		if (ulog_port < 1)
		    ulog_port = DEFAULT_ULOG_PORT;
	    }
	    /* FIXME: we don't resolve host names - only IPs are supported for now */
	}
#ifdef RSERV_DEBUG
	fprintf(stderr, "ULOG: begin %s %s port=%d\n", (u_family == AF_INET) ? "INET" : "UNIX", (u_sock == SOCK_DGRAM) ? "DGRAM" : "STREAM", ulog_port);
#endif
	ulog_sock = socket(u_family, u_sock, 0);
	if (ulog_sock == -1) return;
#if defined O_NONBLOCK && defined F_SETFL
	{ /* try to use non-blocking socket where available */
	    int flags = fcntl(ulog_sock, F_GETFL, 0);
	    if (flags != -1)
		fcntl(ulog_sock, F_SETFL, flags | O_NONBLOCK);
	}
#endif
    }

    /* truncate level to 0..7 */
    if (level > 7)
	level = 7;
    if (level < 0)
	level = 0;
    buf_level = level;

    {
	struct tm *stm;
	time_t now = time(0);
	stm = gmtime(&now);
	strftime (ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", stm);
#ifdef ULOG_MICROTIME
	{   /* this is useful for profiling but breaks the syslog standard */
	    /* FIXME: we could also add sub-second to the "regular"
	       time above as that is legal, but we would lose the delta and
	       linear time. */
	    struct timeval tv;
	    double t;
	    gettimeofday(&tv, 0);
	    t = ((double) tv.tv_sec) + (((double) tv.tv_usec) / 1000000.0);
	    if (time0 < 1.0) timeN = time0 = t;
	    snprintf(ts + strlen(ts), sizeof(ts), "[%.4f/%.4f]", t - time0, t - timeN);
	    timeN = t;
	}
#endif
    }

    /* FIXME: we could cache user/group/pid and show the former
       in textual form ... */
    /* This format is compatible with the syslog format (RFC5424) */
    snprintf(buf, sizeof(buf), "<%d>1 %s %s %s %ld %d/%d - %s: ",
	     level + facility * 8, ts, hn, app_name, 
	     (long) getpid(), (int) getuid(), (int) getgid(),
	     prefix[level]
	     );
    buf_pos = strlen(buf);
    buf_preamble = buf_pos - prefix_len[level] - 2; /* include the prefix */
}

void ulog_add(const char *format, ...) {
    va_list(ap);
    va_start(ap, format);
    if (buf_pos) {
	vsnprintf(buf + buf_pos, sizeof(buf) - buf_pos, format, ap);
	buf_pos += strlen(buf + buf_pos);
    }
    va_end(ap);
}

void ulog_end(void) {
#if defined (RSERV_DEBUG) || defined (ULOG_STDERR)
    buf[buf_pos] = 0;
#ifdef ULOG_MAX_LEVEL
    if (buf_level <= ULOG_MAX_LEVEL)
#endif
#ifdef ULOG_SHORT
    fprintf(stderr, "ULOG: %s\n", buf + buf_preamble);
#else
    fprintf(stderr, "ULOG: %s\n", buf);
#endif
#endif
    if (ulog_sock != -1) {
	if (ulog_port) {
	    struct sockaddr_in sa;
	    memset(&sa, 0, sizeof(sa));
	    sa.sin_family = AF_INET;
	    sa.sin_port = htons(ulog_port);
	    ulog_path[ulog_dcol] = 0;
	    sa.sin_addr.s_addr = inet_addr(ulog_path + 6);
	    ulog_path[ulog_dcol] = ':'; /* we probably don't even need this ... */
	    sendto(ulog_sock, buf, buf_pos, 0, (struct sockaddr*) &sa, sizeof(sa));
	} else {
	    struct sockaddr_un sa;
	    if (!buf_pos) return;
	    memset(&sa, 0, sizeof(sa));
	    sa.sun_family = AF_LOCAL;
	    strcpy(sa.sun_path, ulog_path); /* FIXME: check possible overflow? */
	    sendto(ulog_sock, buf, buf_pos, 0, (struct sockaddr*) &sa, sizeof(sa));
	}
    }
    buf_preamble = buf_pos = 0;
}

void ulog(int level, const char *format, ...) {
    va_list(ap);
    va_start(ap, format);
    ulog_begin(level);
#if defined (RSERV_DEBUG) || defined (ULOG_STDERR)
    /* in case ulog_begin did not start a line */
    if (!buf_pos) {
	*buf = ' ';
	buf_pos = 1;
    }
#endif
    if (buf_pos) {
	vsnprintf(buf + buf_pos, sizeof(buf) - buf_pos, format, ap);
	buf_pos += strlen(buf + buf_pos);
	ulog_end();
    }
    va_end(ap);
}
#endif

#else

void ulog_set_path(const char *path) { }
void ulog_set_app_name(const char *name) { }
void ulog_set_facility(int fac) {}
void ulog_begin(int) {}
void ulog_add(const char *format, ...) { }
void ulog_end(void) {}
void ulog(int, const char *format, ...) { }
void ulog_reset(void) {}
int  ulog_enabled(void) { return 0; }

#endif
