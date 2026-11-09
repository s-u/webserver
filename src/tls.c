#ifndef NO_CONFIG_H
#include "config.h"
#endif

#include "tls.h"
#include "ulog.h"

#ifdef HAVE_TLS

#include <stdio.h>
#include <stdarg.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

struct client_connection {
    client_connection_info_t info;
};

struct tls {
    SSL_CTX *ctx;
    const SSL_METHOD *method;
};

static int first_tls = 1;

static tls_t *tls;

tls_t *shared_tls(tls_t *new_tls) {
    if (!tls)
	tls = new_tls;
    return tls;
}

tls_t *new_tls(void) {
    tls_t *t = (tls_t*) calloc(1, sizeof(tls_t));
    
    if (first_tls) {
	SSL_library_init();
#ifdef RSERV_DEBUG
	SSL_load_error_strings();
#endif
	first_tls = 0;
	tls = t;
    }

    t->method = SSLv23_server_method();
    t->ctx = SSL_CTX_new(t->method);
    return t;
}

int set_tls_pk(tls_t *tls, const char *fn) {
    return SSL_CTX_use_PrivateKey_file(tls->ctx, fn, SSL_FILETYPE_PEM);
}

int set_tls_cert(tls_t *tls, const char *fn) {
    return SSL_CTX_use_certificate_file(tls->ctx, fn, SSL_FILETYPE_PEM);
}

int set_tls_ca(tls_t *tls, const char *fn_ca, const char *path_ca) {
    return SSL_CTX_load_verify_locations(tls->ctx, fn_ca, path_ca);
}

int set_tls_verify(tls_t *tls, int verify) {
    SSL_CTX_set_verify(tls->ctx, verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, 0);
    return 1;
}

static ssize_t tls_recv(client_connection_t *c, void *buf, size_t len) {
    return SSL_read((SSL*) c->info.ssl, buf, len);
}

static ssize_t tls_send(client_connection_t *c, const void *buf, size_t len) {
    return SSL_write((SSL*) c->info.ssl, buf, len);
}

int add_tls(client_connection_t *c, tls_t *tls, int server) {
    SSL *ssl = SSL_new(tls->ctx);
    c->info.ssl = ssl;
    c->info.srv->send = tls_send;
    c->info.srv->recv = tls_recv;
    SSL_set_fd(ssl, c->info.s);
    if (server) 
	return SSL_accept(ssl);
    else
	return SSL_connect(ssl);
}

/* FIXME: when is this used - do we need to worry about ownership transfer of SSL? */
void copy_tls(client_connection_t *src, client_connection_t *dst) {
    dst->info.ssl  = src->info.ssl;
    dst->info.s    = src->info.s;
    dst->info.srv->send = src->info.srv->send;
    dst->info.srv->recv = src->info.srv->recv;
}

void close_tls(client_connection_t *c) {
    if (c->info.ssl) {
	SSL *ssl = (SSL*) c->info.ssl;
	SSL_shutdown(ssl);
	/* NOTE: we are not performing a two-step shutdown. */
	SSL_free(ssl);
	c->info.ssl = 0;
    }
}

void free_tls(tls_t *tls) {
}

/* if cn is present, len > 0 and there is a cert then the common name
   is copied to cn and terminated. It may be truncated if len is too short.
   Return values:
   0 = present but verification failed
   1 = present and verification successful
  -1 = absent */
int verify_peer_tls(client_connection_t *c, char *cn, int len) {
    X509 *peer;
    SSL *ssl = (SSL*) c->info.ssl;
    if ((peer = SSL_get_peer_certificate(ssl))) {
	if (cn && len > 0) {
	    X509_NAME *sn = X509_get_subject_name(peer);
	    X509_NAME_get_text_by_NID(sn, NID_commonName, cn, len);
	    ulog_info("peer cert common name: \"%s\"", cn);
	}
	X509_free(peer);
	if (SSL_get_verify_result(ssl) == X509_V_OK) {
	    ulog_info("peer cert present and OK");
	    return 1;
	} else {
	    ulog_warn("peer cert present, but verification FAILED");
	    return 0;
	}
    }

    ulog_info("INFO: peer nas NO cert");
    return -1;
}

#if 0 /* Can't use stderr in R */
int perror_tls(const char *format, ...) {
    va_list(ap);
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    ERR_print_errors_fp(stderr);
    va_end(ap);
    return 1;
}
#endif

#else /* no SSL/TLS support, ignore everything, fail on everything */

#include <stdio.h>

tls_t *shared_tls(tls_t *new_tls) { return 0; }

tls_t *new_tls(void) { return 0; }
int set_tls_pk(tls_t *tls, const char *fn) { return -1; }
int set_tls_cert(tls_t *tls, const char *fn) { return -1; }
int set_tls_ca(tls_t *tls, const char *fn_ca, const char *path_ca) { return -1; }
int set_tls_verify(tls_t *tls, int verify) { return -1; }
void free_tls(tls_t *tls) { }

int add_tls(client_connection_t *c, tls_t *tls, int server) { return -1; }
void copy_tls(client_connection_t *src, client_connection_t *dst) { }
void close_tls(client_connection_t *c) { }
int verify_peer_tls(client_connection_t *c, char *cn, int len) { return -1; }

#if 0
int perror_tls(const char *format, ...) {
    fprintf(stderr, "ERROR: this binary has been built without SSL/TLS support.\n");
    return 1;
}
#endif

#endif
