#include "server.h"
#include "ulog.h"

#include <string.h>
#include <stdio.h>

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
#else
typedef int socklen_t;
#endif
#include <sys/types.h>
#include <sys/stat.h>
#ifdef FORKED
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

/* provides client socket from accept() to the server so that it can
   modify the socket as needed according to the server flags */
static void accepted_server(client_connection_t *conn) {
	if (!conn || !conn->info.srv)
		return;
#ifdef SO_KEEPALIVE
	/* if keep-alive is enabled and supported - try to set it */
	if (conn->info.srv->flags & SRV_KEEPALIVE) {
		int ka = 1;
		setsockopt(conn->info.s, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof(ka));
	}
#endif
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

#ifdef WIN32
	{
		WSADATA dt;
		/* initialize WinSock 1.1 */
		WSAStartup(0x0101, &dt);
	}
#endif

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

/* FIXME: those are copied from Rserve.c for now - clean it up ! */
static int use_ipv6 = 0;
static int is_child = 0;
static char **allowed_ips;

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

#ifdef FORKED
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

#ifdef FORKED
static int RS_fork(client_connection_t *arg) {
#ifndef WIN32
	return (arg->info.srv && arg->info.srv->fork) ? arg->info.srv->fork(arg) : fork();
#else
	return -1;
#endif
}
#endif

static void accept_process(client_connection_t *srv_conn) {
	/* we may not know the size of args since servers may
	   choose to add fileds, so allocate 1k which is safe */
	client_connection_t *conn = (client_connection_t*)calloc(CLIENT_CONNECTION_ALLOC_SIZE, 1);
	if (!conn) {
		ulog_error("Unable to allocate memory for a new connection");
		return;
	}
	server_t *srv = srv_conn->info.srv;
	socklen_t al = sizeof(conn->info.sa);
#ifndef WIN32
	if (srv->unix_socket) {
		al = sizeof(conn->info.su);
		conn->info.s = accept(srv_conn->info.s, (SA*)&(conn->info.su), &al);
	} else
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
		int i;
		int maxfd = 0;
#ifdef FORKED
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
					al = sizeof(conn->info.sa);
#ifndef WIN32
					if (server[i]->unix_socket) {
						al = sizeof(conn->info.su);
						conn->info.s = accept(ss, (SA*)&(conn->info.su), &al);
					} else
#endif
						conn->info.s = accept(ss, (SA*)&(conn->info.sa), &al);
					if (conn->info.s == INVALID_SOCKET) {
						ulog_error("accept failed: %s", strerror(errno));
						free(conn);
						continue;
					}

					conn->info.srv = srv;
					accepted_server(conn);

					/* FIXME: we want to have a generic way of adding connection filters */
					if (allowed_ips && !srv->unix_socket && !use_ipv6) {
						/* FIXME: IPv6 unconnfe - filtering won't work on IPv6 addresses */
						char **laddr = allowed_ips;
						int allowed = 0;
						while (*laddr)
							if (conn->info.sa.sin_addr.s_addr == inet_addr(*(laddr++)))
								{ allowed=1; break; }
						if (allowed) {
							DBG(ulog_debug("accepted connection for server %p, calling connected", (void*) srv));

							srv->connected(conn);
							succ = 1;
#ifdef FORKED
							/* when the child returns it means it's done (likely an error)
							   but it is forked, so the only right thing to do is to exit */
							if (is_child)
								exit(2);
#endif
						} else {
							/* FIXME: only in debug mode? */
							DBG(ulog_info("peer is not on allowed IP list, closing connection"));
							closesocket(conn->info.s);
							conn->info.s = -1;
						}
					} else { /* ---> remote enabled */
						DBG(ulog_info("accepted connection for server %p, calling connected", (void*) srv));

						srv->connected(conn);
						succ = 1;
						if (is_child) /* same as above */
							exit(2);
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
	end_pool();
}

#include <time.h>

#ifdef FORKED
static pid_t lastChild, parentPID;
#endif

int prepare_child(client_connection_t *args) {
#ifdef FORKED
	if (is_child) return 0; /* this is a no-op if we are already a child
							   FIXME: thould this be an error ? */

    if ((lastChild = RS_fork(args)) != 0) { /* parent/master part */
		int forkErrno = errno; //grab errno close to source before it can be changed by other failures
		/* close the connection socket - the child has it already */
		closesocket(args->info.s);
		if (lastChild == -1)
			ulog_warn("WARNING: fork() failed in prepare_child(): %s", strerror(forkErrno));
		return lastChild;
    }

	/* child part */
	restore_signal_handlers(); /* the handlers handle server shutdown so not needed in the child */

#if 0
	if (main_argv && tag_argv && strlen(main_argv[0]) >= 8)
		strcpy(main_argv[0] + strlen(main_argv[0]) - 8, "/RsrvCHx");
#endif
	is_child = 1;

	if (args->info.srv->flags & SRV_INIT_RNG) { /* this is not great, but better than nothing */
		long rseed = random();
		rseed ^= time(0);
		rseed ^= ((long) getpid()) << 16;
		srandom(rseed);
	}

    parentPID = getppid();
    close_all_srv_sockets(); /* close all server sockets */
#endif

#ifdef CAN_TCP_NODELAY
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
