#ifndef TLS_H__
#define TLS_H__

#include "server.h"

typedef struct tls tls_t;

/* creates a TLS server context. cert is a PEM file with the server
   certificate optionally followed by intermediate certificates, key
   the PEM file with its private key, ca (optional) a PEM file with
   further certificates to send along with the server certificate.
   If client_ca is set, clients must present a valid certificate
   issued by one of the CAs in that PEM file and, if n_cn > 0, its
   common name must match one of the patterns in cn ("name", "*suffix"
   or "prefix*"). Returns NULL on error with a message in err. */
tls_t *new_tls(const char *cert, const char *key, const char *ca,
			   const char *client_ca, const char **cn, int n_cn,
			   char *err, size_t err_len);
void free_tls(tls_t *tls);

/* sets the send/recv functions of the server to the TLS versions */
void tls_server(server_t *srv);

/* sets up the server-side TLS for a new connection, the handshake is
   performed by the first read. Returns 0 on success. */
int  add_tls(client_connection_t *c, tls_t *tls);
void close_tls(client_connection_t *c);
/* returns non-zero if decrypted data can be read without waiting
   for the socket */
int  tls_pending(client_connection_t *c);
/* copies the common name of the verified client certificate of the
   connection into buf (NUL-terminated), returns its length or -1 if
   there is no such certificate (or the name is unsuitable) */
int  tls_client_cn(client_connection_t *c, char *buf, size_t len);

#endif
