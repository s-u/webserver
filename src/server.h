#ifndef RS_SERVER_H__
#define RS_SERVER_H__

#ifndef NO_CONFIG_H
#include "config.h"
#endif

/* this is a voluntary standard flag to request TLS support */
#define SRV_TLS            0x00800
/* this is a voluntary standard flag to request asynchronous behavior */
#define SRV_ASYNC_ACCEPT   0x00200 /* the accept() will be performed on a separate thread */
#define SRV_ASYNC_PROCESS  0x00100 /* recv/send for client connections on a separate thread */
#define SRV_LOG            0x00400 /* enable logging */

/* these flags are global and respected by the default socket server */
#define SRV_IPV6      0x01000 /* use IPv6 */
#define SRV_LOCAL     0x04000 /* bind to local loopback interface only */
#define SRV_KEEPALIVE 0x08000 /* enable keep-alive - note that this is really
								 a client option sice inheritance is not
								 guaranteed */
#define SRV_FORK      0x02000 /* fork on accept (mutually exclusive with
								 SRV_ASYNC_*) */
#define SRV_NONBLOCK  0x10000 /* use non-blocking connections */
#define SRV_INIT_RNG  0x20000 /* request separate RNG for each connection
								 (if not set, forked servers may yield
								 identical RNG chains) */

#include <unistd.h>
#include <stddef.h>

#ifndef WIN32
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/un.h>

#define sockerrno errno

#define SOCKET int
#define INVALID_SOCKET (-1)
#define closesocket(A) close(A)

#else
#define windows
#include <winsock2.h>
#include <windows.h>
#include <string.h>
#include <stdlib.h>
#define inet_aton(A,B) (0, B.s_addr=inet_addr(A))

#define sockerrno WSAGetLastError()

/* Winsock reports errors via WSAGetLastError() with its own codes, so
   the errno constants used to classify socket errors must map to those */
#include <errno.h>
#undef EWOULDBLOCK
#undef EAGAIN
#undef EINTR
#define EWOULDBLOCK WSAEWOULDBLOCK
#define EAGAIN WSAEWOULDBLOCK
#define EINTR WSAEINTR
#endif

#define SA struct sockaddr
#define SAIN struct sockaddr_in

/* size of the space after client_connection_info header
   that can be used by server implementations */
#define CLIENT_CONNECTION_ALLOC_SIZE 1024

/* this structure holds all information pertinent to a single
   accepted connection. The first entry is always guaranteed
   to be struct client_connection_info followed by space
   that is reserved for the implementation and can be used
   in any way. The size of the custom space is defined
   by CLIENT_CONNECTION_ALLOC_SIZE. */
typedef struct client_connection client_connection_t;

/* client connection info flags */
#define CCI_ASYNC_WAIT  4 /* asynchronous processing is requested,
							 no reads are expected while this bit is set */
#define CCI_ASYNC_READY 8 /* asynchronous result is ready */

typedef struct client_connection_info {
	struct server *srv; /* server that instantiated this connection */
    SOCKET s;           /* socket for communication with the client */
	int flags;          /* flags - low 8 bits are reserved */
	void *ssl;          /* pointer for use in SSL/TLS */
	union sau {         /* address of the peer */
		struct sockaddr_in  ip4;
#ifdef HAVE_IPV6
		struct sockaddr_in6 ip6;
#endif
#ifndef WIN32
		struct sockaddr_un  un;
#endif
	} sa;
} client_connection_info_t;

typedef struct server server_t;

typedef void    (*conn_fn_t)(client_connection_t *conn);
typedef int     (*send_fn_t)(client_connection_t *conn, int rsp, size_t len, const void *buf);
typedef ssize_t (*buf_fn_t) (client_connection_t *conn, void *buf, size_t len);
typedef ssize_t (*cbuf_fn_t)(client_connection_t *conn, const void *buf, size_t len);
typedef int     (*fork_fn_t)(client_connection_t *conn);
typedef void    (*srv_fn_t) (server_t *srv);

/* definition of a server */
struct server {
	int ss;               /* server socket */
	int unix_socket;      /* 0 = TCP/IP, 1 = unix socket */
	int flags;            /* optional server-specific flags */
	int res;              /* -- reserved -- */
	conn_fn_t connected;  /* function called for each new connection
							 NOTE: it receives the ownership of the client_connection_t
							 so it has to free it when it is no longer needed */
	send_fn_t send_resp;  /* send response */
	cbuf_fn_t send;       /* direct send */
	buf_fn_t  recv;       /* direct receive */
	fork_fn_t fork;       /* fork */
	conn_fn_t process;    /* processing iteration */
	conn_fn_t conn_fin;   /* connection finalizer */
	srv_fn_t  fin;        /* optional server finalization */
	struct server *parent;/* parent server - used only by multi-layer servers */
	struct conn_filter *filter;  /* connection filter */
	void   *aux;          /* auxiliary data; if used, you may want to set fin above */
};

server_t *create_server(int port, const char *localSocketName, int localSocketMode, int flags);
int add_server(server_t *srv);
int rm_server(server_t *srv);

int server_get_info(server_t *srv, client_connection_info_t *info);

/* creates a virtual server that is used to accept connections */
server_t *create_accept_server(server_t *srv);

/* server stacks */
typedef struct server_stack server_stack_t;
server_stack_t* create_server_stack(void);
void push_server(server_stack_t *s, server_t *srv);
int  server_stack_size(server_stack_t *s);
void release_server_stack(server_stack_t *s);

/* some generic implementations */
ssize_t server_recv(client_connection_t *conn, void *buf, size_t len);
ssize_t server_send(client_connection_t *conn, const void *buf, size_t len);

void stop_server_loop(void);
void serverLoop(void);


ssize_t send_all(client_connection_t *c, const void *buf, size_t len, double timeout,
				 int (*on_intr)(client_connection_t *c));

/* helper function that prepares the process just like Rserve
   internal impleemntation - forking when desired, establishing
   pipes, setting see, uid/gid, cwd etc.
   returns 0 for the child */
int prepare_child(client_connection_t *arg);

/* set blocking flag on a connection. Note that this should only
   be used by implementations of virtual servers, since real
   servers set the blocking behavior based on the SRV_NONBLOCK
   server flag.
   Returns 0 on success.
*/
int client_set_blocking(client_connection_t *conn, int block);

/* this one is called by the former to close all server sockets in the child */
void close_all_srv_sockets(void);
void close_server_sockets_in_child(int keep);

#endif

/*--- The following makes the indenting behavior of emacs compatible
      with Xcode's 4/4 setting ---*/
/* Local Variables: */
/* indent-tabs-mode: t */
/* tab-width: 4 */
/* c-basic-offset: 4 */
/* End: */
