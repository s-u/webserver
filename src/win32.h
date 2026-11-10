#ifndef WIN32_H__
#define WIN32_H__

#ifdef WIN32

/* windwos socket compatibitiy */
#include "server.h"

#include <io.h>
#include <fcntl.h>
#include <time.h>
#include <winsock2.h>

typedef unsigned long int nfds_t;
typedef int socklen_t;

#define poll WSAPoll
#define SHUT_RD SD_RECEIVE
#define SHUT_WR SD_SEND
#define SHUT_RDWR SD_BOTH

/* timespec_get() is C11, older Windows runtimes don't have it */
#ifndef TIME_UTC
#define TIME_UTC 1
#define NEED_TIMESPEC_GET 1
int timespec_get(struct timespec *ts, int base);
#endif

int pipe(SOCKET fds[2]);

#endif

#endif
