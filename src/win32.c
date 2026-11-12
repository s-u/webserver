/* Windows compatibility functions */

#include "win32.h"

#ifdef WIN32

#include "ulog.h"
#include "server.h"

#ifdef NEED_TIMESPEC_GET
int timespec_get(struct timespec *ts, int base) {
    __int64 wintime;
    GetSystemTimeAsFileTime((FILETIME*)&wintime);
    wintime    -= 116444736000000000LL;  /* 1601/1/1 to epoch */
    ts->tv_sec  = wintime / 10000000LL;
    ts->tv_nsec = wintime % 10000000LL * 100;
    return base;
}
#endif

#ifndef SOCKADDR
#define SOCKADDR struct sockaddr
#endif

#include <stdio.h>
#include <string.h>

static int X(const char *where, int res) {
    if (res)
	ulog_error("%s failed: %d", where, WSAGetLastError());
    return res;
}

/* Windows doesn't support select/poll on pipes, neither does it provide
   socketpair() so we have to create two sockets and connect them */
int pipe(SOCKET fds[2]) {
    SOCKET s1, s2, s3;
    struct sockaddr_in sbind;
    socklen_t len;
    /* pipe() may be used before any server has initialized Winsock */
    if (initSock()) return -1;
    s1 = socket(AF_INET, SOCK_STREAM, 0);
    if (s1 == INVALID_SOCKET) return -1;
    sbind.sin_family = AF_INET;
    sbind.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sbind.sin_port = 0; /* any port */
    len = sizeof(sbind);
    if (X("bind", bind(s1, (SOCKADDR*) &sbind, sizeof(sbind))) ||
	X("getsockname", getsockname(s1, (SOCKADDR*) &sbind, &len)) ||
	X("listen", listen(s1, 4))) {
	closesocket(s1);
	return -1;
    }
    if ((s2 = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET) {
	closesocket(s1);
	return -1;
    }
    if (X("connect", connect(s2, (SOCKADDR*) &sbind, len))) {
	closesocket(s1);
	closesocket(s2);
	return -1;
    }
    ulog_debug("pipe connected");
    len = sizeof(sbind);
    s3 = accept(s1, (SOCKADDR*) &sbind, &len);
    ulog_debug("pipe: accept = %d", (int) s3);
    if (s3 == INVALID_SOCKET) {
	closesocket(s1);
	closesocket(s2);
	return -1;
    }
    ulog_debug("pipe: sockets connected (%d<->%d)", (int) s2, (int) s3);
    /* no more listening */
    closesocket(s1);
    fds[0] = s2;
    fds[1] = s3;
    return 0;
}

#else
/* ISO C does not allow an empty translation unit */
typedef int win32_c_unused;
#endif
