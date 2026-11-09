#include "server.h"
#include "cfilter.h"
#include "ulog.h"

#include <string.h>
#include <stdio.h>
#include <fcntl.h>

#include "dalloc.h"

#define SOCK_ERRORS
#define LISTENQ 32

#ifdef RSERV_DEBUG
#define DBG(X) X
#else
#define DBG(X) while(0) {}
#endif

/* all connections must start with the connection_info structure */
struct client_connection {
	client_connection_info_t info;
};

#ifndef WIN32
#include <sys/un.h> /* needed for unix sockets */
#define initSock() do{} while(0)
#else
typedef int socklen_t;
static WSADATA wsaData;
int sock_init_done;
static int initSock() {
	if (sock_init_done)
		return 0;
	if (WSAStartup(0x200, &wsaData) && WSAStartup(0x101, &wsaData))
		return 1;
	sock_init_done = 1;
	return 0;
}
#endif
#include <sys/types.h>
#include <sys/stat.h>
#ifndef WIN32
#include <sys/wait.h>
#endif

/* AF_LOCAL is the POSIX version of AF_UNIX - we need this e.g. for AIX */
#ifndef AF_LOCAL
#define AF_LOCAL AF_UNIX
#endif

/* keep track of all bound server sockets so they can be easily all closed after fork
   this is important for two reasons: so ports don't get stuck bound after the server
   has been shut down but children are still around, and so that a rogue child cannot
   impersonate the server after the server has been shut down (since the port may have
   been bound at higher privileges than the child may have at this point) */
#define MAX_SRVS 512
static int active_srv_sockets[MAX_SRVS];

static int active = 1;
#ifdef WIN32
#include <Rinternals.h>

static void check_interrupt(void *data) {
	R_CheckUserInterrupt();
}

/* no signals on Windows: poll R for a pending user interrupt (Ctrl-C/Esc).
   R_CheckUserInterrupt() jumps out when interrupted, so catch that to be
   able to shut down cleanly. Must be called on the main thread. */
static int interrupt_pending(void) {
	return !R_ToplevelExec(check_interrupt, 0);
}
#endif

static void add_active_srv_socket(int s) {
	int i = 0;
	while (active_srv_sockets[i] && i < MAX_SRVS) {
		if (active_srv_sockets[i] == s) return;
		i++;
	}
	if (i < MAX_SRVS)
		active_srv_sockets[i] = s;
}

static void rm_active_srv_socket(int s) {
	int i = 0;
	while (i < MAX_SRVS) {
		if (active_srv_sockets[i] == s) {
			active_srv_sockets[i] = 0;
			break;
		}
		i++;
	}
}

/* this is typically used after fork in the child process */
void close_all_srv_sockets(void) {
	int i = 0;
	while (i < MAX_SRVS) {
		if (active_srv_sockets[i]) closesocket(active_srv_sockets[i]);
		i++;
	}
}

static int set_blocking(SOCKET s, int block) {
#ifdef WIN32
	unsigned long mode = block ? 0 : 1;
	return ioctlsocket(s, FIONBIO, &mode);
#else
	int flags = fcntl(s, F_GETFL, 0);
	if (flags == -1) return -1;
	flags = block ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
	return fcntl(s, F_SETFL, flags);
#endif
}

/* provides client socket from accept() to the server so that it can
   modify the socket as needed according to the server flags */
static void accepted_server(client_connection_t *conn) {
	if (!conn || !conn->info.srv)
		return;
#ifdef SO_KEEPALIVE
	/* if keep-alive is enabled and supported - try to set it */
	if (conn->info.srv->flags & SRV_KEEPALIVE) {
		int ka = 1;
		setsockopt(conn->info.s, SOL_SOCKET, SO_KEEPALIVE, (const char*) &ka, sizeof(ka));
	}
#endif
	if (conn->info.srv->flags & SRV_NONBLOCK)
		set_blocking(conn->info.s, 0);
}

int client_set_blocking(client_connection_t *conn, int block) {
	if (!conn)
		return -1;
	return set_blocking(conn->info.s, block);
}

#define CR_ERR  4
#define CR_SEND 2
#define CR_RECV 1

/* return -1 on error, 0 on timeout and mask of what if ready */
int client_ready(client_connection_t *c, int what, double timeout) {
	fd_set sfs, rfs, efs;
    struct timeval tv;
	int n, fd = c->info.s;
	if (what & CR_RECV) {
		FD_ZERO(&rfs);
		FD_SET(fd, &rfs);
	}
	if (what & CR_SEND) {
		FD_ZERO(&sfs);
		FD_SET(fd, &sfs);
	}
	if (what & CR_ERR) {
		FD_ZERO(&efs);
		FD_SET(fd, &efs);
	}
	tv.tv_sec  = (int) timeout;
	tv.tv_usec = (int) ((timeout - tv.tv_sec) * 1000000.0);
	n = select(fd + 1,
			   (what & CR_RECV) ? &rfs : 0,
			   (what & CR_SEND) ? &sfs : 0,
			   (what & CR_ERR)  ? &efs : 0,
			   (timeout < 0) ? 0 : &tv);
	if (n == 1)
		return (((what & CR_RECV) && FD_ISSET(fd, &rfs)) ? CR_RECV : 0) |
			(((what & CR_SEND) && FD_ISSET(fd, &sfs)) ? CR_SEND : 0) |
			(((what & CR_ERR) && FD_ISSET(fd, &efs)) ? CR_ERR : 0);
	return n;
}
/* -1 = send error, -2 = timeout, -3 = on_intr signalled, -4 = clonnection closed */
ssize_t send_all(client_connection_t *c, const void *buf, size_t len, double timeout,
				 int (*on_intr)(client_connection_t *c)) {
    ssize_t total = len;
    if (!buf || !len || !c || !c->info.srv)
		return 0;
    server_t *srv = c->info.srv;
    while (len) {
		ssize_t n = srv->send(c, buf, len);
		if (n == 0)
			return -4; /* since we cannot resume, we don't care about size sent */
		if (n < 0) {
			if (sockerrno == EAGAIN || sockerrno == EWOULDBLOCK) {
				DBG(ulog_debug("sever.send_all: EAGAIN on %p (%d)", (void*) c, c->info.s));
				int w = client_ready(c, CR_SEND | CR_ERR, timeout);
				if (!w) /* timeout */
					return -2;
				/* aything else we let send() handle */
				continue;
			}
			if (sockerrno == EINTR) {
				if (on_intr && on_intr(c))
					return -3;
				continue;
			}
			return -1;
		}
		len -= n;
		buf = (const char*) buf + n;
    }
    return total;
}

int server_get_info(server_t *srv, client_connection_info_t *info) {
	socklen_t len = sizeof(struct sockaddr_in);
	if (!info || !srv)
		return -1;
	info->srv = srv;
	info->s = srv->ss;
#ifdef HAVE_IPV6
	if (srv->flags & SRV_IPV6)
		len = sizeof(info->sa.ip6);
#endif
#ifndef WIN32
	if (srv->unix_socket)
		len = sizeof(info->sa.un);
#endif
	if (getsockname(srv->ss, (struct sockaddr*) &info->sa, &len))
		return -2;
	return 0;
}

server_t *create_server(int port, const char *localSocketName, int localSocketMode, int flags) {
	server_t *srv;
	SAIN ssa;
	int reuse, ss;
#ifdef HAVE_IPV6
	struct sockaddr_in6 ssa6;
#endif

#ifndef WIN32
	struct sockaddr_un lusa;
#endif

	DBG(ulog_debug(" - create_server(port = %d, socket = %s, mode = %d, flags = 0x%x)", port, localSocketName ? localSocketName : "<NULL>", localSocketMode, flags));

	initSock();

	if (localSocketName) {
#ifdef WIN32
		ulog_error("Local sockets are not supported on non-unix systems.");
		return 0;
#else
		if ((ss = socket(AF_LOCAL, SOCK_STREAM, 0)) == -1) {
			ulog_error("cannot create local socket: %s", strerror(errno));
			return 0;
		}

		memset(&lusa, 0, sizeof(lusa));
		lusa.sun_family = AF_LOCAL;
		if (strlen(localSocketName) > sizeof(lusa.sun_path) - 2) {
			ulog_error("ERROR: Local socket name is too long for this system.");
			return 0;
		}
		strcpy(lusa.sun_path, localSocketName);
		remove(localSocketName); /* remove existing if possible */
#endif
	} else
#ifdef HAVE_IPV6
		ss = socket((flags & SRV_IPV6) ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
#else
	    ss = socket(AF_INET, SOCK_STREAM, 0);
#endif

	if (ss == INVALID_SOCKET) {
		ulog_error("no available socket: %s", strerror(errno));
		return 0;
	}

	srv = (server_t*) calloc(1, sizeof(server_t));
	if (!srv) {
		ulog_error("cannot allocate memory for server structure");
		return 0;
	}

	srv->ss = ss;
	srv->unix_socket = localSocketName ? 1 : 0;
	srv->flags = flags;
	srv->parent = 0;

	reuse = 1; /* enable socket address reusage */
	setsockopt(ss, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

#ifndef WIN32
	if (localSocketName) {
		if (bind(ss, (SA*) &lusa, sizeof(lusa))) {
			ulog_error("unable to bind to %s: %s", lusa.sun_path, strerror(errno));
			return 0;
		}
		if (localSocketMode)
			chmod(localSocketName, localSocketMode);
	} else {
#endif
#ifdef HAVE_IPV6
		if (flags & SRV_IPV6) {
			memset(&ssa6, 0, sizeof(ssa6));
			ssa6.sin6_family = AF_INET6;
			ssa6.sin6_port = htons(port);
			ssa6.sin6_addr = (flags & SRV_LOCAL) ? in6addr_loopback : in6addr_any;
			if (bind(ss, (struct sockaddr*) &ssa6, sizeof(ssa6))) {
				ulog_error("unable to bind to IPv6 port %d: %s", port, strerror(errno));
				closesocket(ss);
				return 0;
			}
		} else {
#endif
			memset(&ssa, 0, sizeof(ssa));
			ssa.sin_family = AF_INET;
			ssa.sin_port = htons(port);
			ssa.sin_addr.s_addr = htonl((flags & SRV_LOCAL) ? INADDR_LOOPBACK : INADDR_ANY);
			if (bind(ss, (struct sockaddr*) &ssa, sizeof(ssa))) {
				ulog_error("unable to bind to IPv4 port %d: %s", port, strerror(errno));
				closesocket(ss);
				return 0;
			}
#ifdef HAVE_IPV6
		} /* if (flags & SRV_IPV6) else */
#endif
#ifndef WIN32
	} /* if (localSocketName) else */
#endif
    
	if (listen(ss, LISTENQ)) {
		ulog_error("listen failed: %s", strerror(errno));
		closesocket(ss);
		return 0;
	}

	add_active_srv_socket(ss);

	return srv;
}

static void server_fin(server_t *srv) {
	if (srv) {
		if (srv->ss != -1) {
			closesocket(srv->ss);
			rm_active_srv_socket(srv->ss);
			srv->ss = -1;
		}
		if (srv->fin)
			srv->fin(srv);
	}
}

#define NSPS 16
struct server_stack {
	server_stack_t *prev, *next;
	int ns;
	server_t *srv[NSPS];
};

server_stack_t* create_server_stack(void) {
	server_stack_t *s = (server_stack_t*) malloc(sizeof(server_stack_t));
	s->prev = s->next = 0;
	s->ns = 0;
	return s;
}

void push_server(server_stack_t *s, server_t *srv) {
	while (s->ns >= NSPS && s->next) s = s->next;
	if (s->ns >= NSPS) {
		server_stack_t *ns = create_server_stack();
		ns->prev = s;
		s = s->next = ns;
	}
	s->srv[s->ns++] = srv;
}

void release_server_stack(server_stack_t *s) {
	while (s && s->next) s = s->next;
	while (s) {
		int i = s->ns;
		while (i-- > 0) {
			rm_server(s->srv[i]);
			free(s->srv[i]);
		}
		s->ns = 0;
		s = s->prev;
	}
}

int server_stack_size(server_stack_t *s) {
	int n = 0;
	while (s) {
		n += s->ns;
		s = s->next;
	}
	return n;
}

#define MAX_SERVERS 128
static int servers;
static server_t *server[MAX_SERVERS];

int add_server(server_t *srv) {
	if (!srv) return 0;
	if (servers >= MAX_SERVERS) {
		ulog_error("too many servers");
		return 0;
	}
	server[servers++] = srv;

	DBG(ulog_debug("adding server %p (total %d servers)", (void*) srv, servers));

	return 1;
}

int rm_server(server_t *srv) {
	int i = 0;
	if (!srv) return 0;
	while (i < servers) {
		if (server[i] == srv) {
			int j = i + 1;
			while (j < servers) { server[j - 1] = server[j]; j++; }
			servers--;
		} else i++;
	}
	server_fin(srv);

	DBG(ulog_debug("removing server %p (total %d servers left)", (void*) srv, servers));

	return 1;
}

#ifndef WIN32
/* Closes (in a forked worker) every socket that belongs to one of our servers
   except `keep`: the listening sockets and the accepted client connections,
   which share the local address of the listening socket. Otherwise the worker
   would keep client connections open after the parent has closed them and
   keep the port bound after the server has stopped. Other descriptors (e.g.
   database connections of the R session) are left alone. */
void close_server_sockets_in_child(int keep) {
	long maxfd = sysconf(_SC_OPEN_MAX);
	int fd, i;
	if (maxfd < 0 || maxfd > 65536) maxfd = 65536;
	for (fd = 3; fd < maxfd; fd++) {
		struct sockaddr_storage la;
		socklen_t ll = sizeof(la);
		if (fd == keep || getsockname(fd, (struct sockaddr*) &la, &ll))
			continue; /* not an open socket */
		for (i = 0; i < servers; i++) {
			struct sockaddr_storage sa;
			socklen_t sl = sizeof(sa);
			int match = 0;
			if (!server[i] || server[i]->ss == -1 || getsockname(server[i]->ss, (struct sockaddr*) &sa, &sl))
				continue;
			if (la.ss_family != sa.ss_family)
				continue;
			if (la.ss_family == AF_INET)
				match = ((struct sockaddr_in*) &la)->sin_port == ((struct sockaddr_in*) &sa)->sin_port;
#ifdef AF_INET6
			else if (la.ss_family == AF_INET6)
				match = ((struct sockaddr_in6*) &la)->sin6_port == ((struct sockaddr_in6*) &sa)->sin6_port;
#endif
			else if (la.ss_family == AF_UNIX) {
				const char *lp = ((struct sockaddr_un*) &la)->sun_path, *sp = ((struct sockaddr_un*) &sa)->sun_path;
				match = *lp && *sp && !strcmp(lp, sp);
			}
			if (match) {
				close(fd);
				break;
			}
		}
	}
}
#else
void close_server_sockets_in_child(int keep) {}
#endif

static int is_child = 0;

ssize_t server_recv(client_connection_t *arg, void *buf, size_t len) {
	return recv(arg->info.s, buf, len, 0);
}

ssize_t server_send(client_connection_t *arg, const void *buf, size_t len) {
	return send(arg->info.s, buf, len, 0);
}

typedef void (*sig_fn_t)(int);

#ifndef WIN32

#include <signal.h>

/* NULL ptr is used on some systems as SIG_DFL so we have
   to define our own value for "not set" */
static void sig_not_set(int x) {}

static void sigHandler(int i) {
	active = 0;
}

sig_fn_t old_HUP = sig_not_set, old_TERM = sig_not_set, old_INT = sig_not_set;
sig_fn_t old_PIPE = sig_not_set;

static void block_SIGPIPE(void) {
	if (old_PIPE == sig_not_set) old_PIPE = signal(SIGPIPE, SIG_IGN);
}

static void restore_SIGPIPE(void) {
	if (old_PIPE != sig_not_set) {
		signal(SIGPIPE, old_PIPE);
		old_PIPE = sig_not_set;
	}
}

static void setup_signal_handlers(void) {
	if (old_HUP == sig_not_set)  old_HUP  = signal(SIGHUP,  sigHandler);
	if (old_TERM == sig_not_set) old_TERM = signal(SIGTERM, sigHandler);
	if (old_INT == sig_not_set)  old_INT  = signal(SIGINT,  sigHandler);
}

#ifndef WIN32
static void restore_signal_handlers(void) {
	if (old_HUP != sig_not_set) {
		signal(SIGHUP, old_HUP);
		old_HUP = sig_not_set;
	}
	if (old_TERM != sig_not_set) {
		signal(SIGTERM, old_TERM);
		old_TERM = sig_not_set;
	}
	if (old_INT != sig_not_set) {
		signal(SIGINT, old_INT);
		old_INT = sig_not_set;
	}
}
#endif
#else
static void setup_signal_handlers(void) {
}
static void restore_signal_handlers(void) {
}
static void block_SIGPIPE(void) {
}
static void restore_SIGPIPE(void) {
}
#endif

static int RS_fork(client_connection_t *arg) {
#ifndef WIN32
	return (arg->info.srv && arg->info.srv->fork) ? arg->info.srv->fork(arg) : fork();
#else
	return -1;
#endif
}

static void accept_process(client_connection_t *srv_conn) {
	/* we may not know the size of args since servers may
	   choose to add fileds, so allocate 1k which is safe */
	client_connection_t *conn = (client_connection_t*)calloc(CLIENT_CONNECTION_ALLOC_SIZE, 1);
	if (!conn) {
		ulog_error("Unable to allocate memory for a new connection");
		return;
	}
	server_t *srv = srv_conn->info.srv;
	socklen_t al = sizeof(conn->info.sa.ip4);
#ifdef HAVE_IPV6
	if (srv->flags & SRV_IPV6)
		al = sizeof(conn->info.sa.ip6);
#endif
#ifndef WIN32
	if (srv->unix_socket)
		al = sizeof(conn->info.sa.un);
#endif
	conn->info.s = accept(srv_conn->info.s, (SA*)&(conn->info.sa), &al);
	if (conn->info.s == INVALID_SOCKET) {
		ulog_error("accept failed: %s", strerror(errno));
		free(conn);
		return;
	}
	conn->info.srv = srv->parent;
	accepted_server(conn);
	DBG(ulog_info("accepted connection for server %p (async, flags 0x%x), new socket %d, calling connected",
				  (void*) srv->parent, srv->parent->flags, conn->info.s));
	conn->info.srv->connected(conn);
}

server_t *create_accept_server(server_t *srv) {
	server_t *asrv = (server_t*) calloc(sizeof(server_t), 1);
	if (!asrv)
		return asrv;
	asrv->unix_socket = srv->unix_socket;
	asrv->process = accept_process;
	asrv->parent = srv;
	return asrv;
}

void stop_server_loop(void) {
	/* FIXME: do we need to send a signal?
	   This is not actually used so far, so we can think about it ... */
	active = 0;
}

void serverLoop(void) {
    struct timeval timv;
    int selRet = 0;
    fd_set readfds;

	active = 1; /* starting it means we are active */

	begin_pool("serverLoop");

	setup_signal_handlers();
	block_SIGPIPE();

    while(active && servers) { /* main serving loop */
#ifdef WIN32
	if (interrupt_pending()) {
		ulog_info("serverLoop: interrupted");
		break;
	}
#endif
		int i;
		int maxfd = 0;
#ifndef WIN32
		/* FIXME: always? */
		while (waitpid(-1, 0, WNOHANG) > 0);
#endif
		/* 500ms (used to be 10ms) - it shouldn't really matter since
		   it's ok for us to sleep -- the timeout will only influence
		   how often we collect terminated children and (maybe) how
		   quickly we react to shutdown */
		timv.tv_sec = 0; timv.tv_usec = 500000;
		FD_ZERO(&readfds);
		for (i = 0; i < servers; i++)
			if (server[i] && !(server[i]->flags & SRV_ASYNC_ACCEPT))
				{
					int ss = server[i]->ss;
					if (ss > maxfd)
						maxfd = ss;
					FD_SET(ss, &readfds);
				}

		selRet = select(maxfd + 1, &readfds, 0, 0, &timv);

		if (selRet > 0) {
			for (i = 0; i < servers; i++) {
				socklen_t al;
				server_t *srv = server[i];
				int ss = srv->ss;
				int succ = 0;
				if (server[i] && FD_ISSET(ss, &readfds)) {
					/* we may not know the size of args since servers may
					   choose to add fileds, so allocate 1k which is connfe */
					client_connection_t *conn = (client_connection_t*)calloc(CLIENT_CONNECTION_ALLOC_SIZE, 1);
					al = sizeof(conn->info.sa.ip4);
#ifdef HAVE_IPV6
					if (server[i]->flags & SRV_IPV6)
						al = sizeof(conn->info.sa.ip6);
#endif
#ifndef WIN32
					if (server[i]->unix_socket)
						al = sizeof(conn->info.sa.un);
#endif
					conn->info.s = accept(ss, (SA*)&(conn->info.sa), &al);
					if (conn->info.s == INVALID_SOCKET) {
						ulog_error("accept failed: %s", strerror(errno));
						free(conn);
						continue;
					}

					conn->info.srv = srv;
					accepted_server(conn);

					/* is this connection allowed? */
					if (!srv->filter || srv->unix_socket || conn_filter_allowed(srv->filter, &conn->info)) {
						DBG(ulog_debug("serverLoop: accepted connection for server %p, calling connected", (void*) srv));

						srv->connected(conn);
						succ = 1;
#ifndef WIN32
						/* when the child returns it means it's done,
						   but it is forked, so the only right thing to do is to exit */
						if ((srv->flags & SRV_FORK) && is_child) {
							DBG(ulog_debug("serverLoop: child pid=%d done, terminating", (int) getpid()));
							exit(2);
						}
#endif
					} else {
						/* FIXME: log only in debug mode? */
						DBG(ulog_info("serverLoop: peer did not pass connection filter, closing connection"));
						closesocket(conn->info.s);
						conn->info.s = -1;
					}
					/* connected assumes ownership (in case it is asynchronous)
					   so it is expected to free the args - hence free only if
					   it was not called */
					if (!succ)
						free(conn);
				} /* ready server */
			} /* severs loop */
		} /* select */
    } /* end while(active) */

	restore_SIGPIPE();
	/* give SIGINT/SIGTERM/SIGHUP back to R, otherwise Ctrl-C would keep
	   just clearing our `active' flag after the server has stopped */
	restore_signal_handlers();
	end_pool();
}

#ifndef WIN32
static pid_t parentPID;
#endif

int prepare_child(client_connection_t *args) {
#ifndef WIN32
	if (args && args->info.srv && (args->info.srv->flags & SRV_FORK)) {
		pid_t lastChild;
		if (is_child) return 0; /* this is a no-op if we are already a child
								   FIXME: should this be an error ? */

		if ((lastChild = RS_fork(args)) != 0) { /* parent/master part */
			int forkErrno = errno; /* grab errno close to source before it can be changed by other failures */
			/* close the connection socket - the child has it already */
			closesocket(args->info.s);
			if (lastChild == -1)
				ulog_warn("fork() failed in prepare_child(): %s", strerror(forkErrno));
			return lastChild;
		}

		/* child part */
		restore_signal_handlers(); /* the handlers handle server shutdown so not needed in the child */

#if 0
		if (main_argv && tag_argv && strlen(main_argv[0]) >= 8)
			strcpy(main_argv[0] + strlen(main_argv[0]) - 8, "/RsrvCHx");
#endif
		is_child = 1;

		parentPID = getppid();
		close_all_srv_sockets(); /* close all server sockets */
	}
#endif /* !WIN32 */

#ifdef TCP_NODELAY
    {
     	int opt = 1;
        setsockopt(args->info.s, IPPROTO_TCP, TCP_NODELAY, (const char*) &opt, sizeof(opt));
    }
#endif

	return 0;
}

/*--- The following makes the indenting behavior of emacs compatible
      with Xcode's 4/4 setting ---*/
/* Local Variables: */
/* indent-tabs-mode: t */
/* tab-width: 4 */
/* c-basic-offset: 4 */
/* End: */
