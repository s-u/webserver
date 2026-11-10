#ifndef NO_CONFIG_H
#include "config.h"
#endif

#include "tls.h"
#include "ulog.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef HAVE_TLS

#include <limits.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

struct client_connection {
    client_connection_info_t info;
};

struct tls {
    SSL_CTX *ctx;
    char **cn;  /* allowed client common names (patterns) */
    int n_cn;
};

void free_tls(tls_t *tls) {
    if (tls) {
	int i;
	if (tls->ctx)
	    SSL_CTX_free(tls->ctx);
	for (i = 0; i < tls->n_cn; i++)
	    free(tls->cn[i]);
	free(tls->cn);
	free(tls);
    }
}

/* matches a common name against a pattern: "*suffix", "prefix*" or exact */
static int cn_match(const char *pat, const char *cn, size_t len) {
    size_t pl = strlen(pat);
    if (pl && *pat == '*')
	return (len >= pl - 1 && !memcmp(cn + len - (pl - 1), pat + 1, pl - 1));
    if (pl && pat[pl - 1] == '*')
	return (len >= pl - 1 && !memcmp(cn, pat, pl - 1));
    return (len == pl && !memcmp(cn, pat, pl));
}

/* checks the common name of the client certificate against the
   allowed names, the certificate must have exactly one CN */
static int client_cn_ok(tls_t *tls, X509 *cert) {
    X509_NAME *sn = X509_get_subject_name(cert);
    ASN1_STRING *d;
    unsigned char *cn = 0;
    int i, len, ok = 0;
    if (!sn || (i = X509_NAME_get_index_by_NID(sn, NID_commonName, -1)) < 0) {
	ulog_info("TLS: client certificate has no common name, rejecting");
	return 0;
    }
    if (X509_NAME_get_index_by_NID(sn, NID_commonName, i) >= 0) {
	ulog_info("TLS: client certificate has more than one common name, rejecting");
	return 0;
    }
    d = X509_NAME_ENTRY_get_data(X509_NAME_get_entry(sn, i));
    if ((len = ASN1_STRING_to_UTF8(&cn, d)) < 0)
	return 0;
    /* embedded NULs could be used to fake a match */
    if (strlen((const char*) cn) == (size_t) len)
	for (i = 0; i < tls->n_cn; i++)
	    if (cn_match(tls->cn[i], (const char*) cn, len)) {
		ok = 1;
		break;
	    }
    if (!ok)
	ulog_info("TLS: client certificate common name \"%s\" is not allowed, rejecting", cn);
    OPENSSL_free(cn);
    return ok;
}

static int verify_cb(int preverify_ok, X509_STORE_CTX *x) {
    SSL *ssl;
    tls_t *tls;
    /* only the client certificate itself is subject to the CN check */
    if (!preverify_ok || X509_STORE_CTX_get_error_depth(x) > 0)
	return preverify_ok;
    ssl = (SSL*) X509_STORE_CTX_get_ex_data(x, SSL_get_ex_data_X509_STORE_CTX_idx());
    tls = ssl ? (tls_t*) SSL_CTX_get_app_data(SSL_get_SSL_CTX(ssl)) : 0;
    if (tls && tls->n_cn && !client_cn_ok(tls, X509_STORE_CTX_get_current_cert(x))) {
	X509_STORE_CTX_set_error(x, X509_V_ERR_APPLICATION_VERIFICATION);
	return 0;
    }
    return 1;
}

/* we cannot ask for a password, so encrypted keys fail (and we note it) */
static int no_password(char *buf, int size, int rwflag, void *u) {
    if (u)
	*((int*) u) = 1;
    return 0;
}

/* fills err with the message, the file name and the OpenSSL error, frees tls */
static tls_t *tls_fail(tls_t *tls, char *err, size_t err_len,
		       const char *msg, const char *fn) {
    unsigned long e = ERR_peek_last_error();
    char ebuf[256];
    ebuf[0] = 0;
    if (e)
	ERR_error_string_n(e, ebuf, sizeof(ebuf));
    snprintf(err, err_len, "%s%s%s%s%s%s", msg, fn ? " '" : "", fn ? fn : "", fn ? "'" : "",
	     e ? ": " : "", ebuf);
    ERR_clear_error();
    free_tls(tls);
    return 0;
}

tls_t *new_tls(const char *cert, const char *key, const char *ca,
	       const char *client_ca, const char **cn, int n_cn,
	       char *err, size_t err_len) {
    tls_t *tls = (tls_t*) calloc(1, sizeof(tls_t));
    int pw_asked = 0, i;
    SSL_CTX *ctx;

    ERR_clear_error();
    if (!tls)
	return tls_fail(tls, err, err_len, "cannot allocate TLS context", 0);
    if (!(tls->ctx = ctx = SSL_CTX_new(TLS_server_method())))
	return tls_fail(tls, err, err_len, "cannot create TLS context", 0);
    SSL_CTX_set_app_data(ctx, tls);
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
#ifdef SSL_OP_NO_RENEGOTIATION
    SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION);
#endif
    /* SSL_write() behaves like send() on non-blocking sockets and the
       chain sent is exactly the one we have been given */
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER |
		     SSL_MODE_NO_AUTO_CHAIN);
    SSL_CTX_set_session_id_context(ctx, (const unsigned char*) "webserver", 9);

    if (SSL_CTX_use_certificate_chain_file(ctx, cert) != 1)
	return tls_fail(tls, err, err_len, "cannot load certificate from", cert);

    SSL_CTX_set_default_passwd_cb(ctx, no_password);
    SSL_CTX_set_default_passwd_cb_userdata(ctx, &pw_asked);
    i = SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM);
    SSL_CTX_set_default_passwd_cb_userdata(ctx, 0);
    if (i != 1) {
	unsigned long e = ERR_peek_last_error();
	return tls_fail(tls, err, err_len, pw_asked ?
			"encrypted private keys are not supported, cannot load key from" :
			(ERR_GET_LIB(e) == ERR_LIB_X509 && ERR_GET_REASON(e) == X509_R_KEY_VALUES_MISMATCH) ?
			"private key does not match the certificate, key" :
			"cannot load private key from", key);
    }
    if (SSL_CTX_check_private_key(ctx) != 1)
	return tls_fail(tls, err, err_len, "private key does not match the certificate, key", key);

    if (ca) { /* additional certificates sent along */
	BIO *b = BIO_new_file(ca, "r");
	X509 *x;
	unsigned long e;
	int n = 0;
	if (!b)
	    return tls_fail(tls, err, err_len, "cannot open", ca);
	while ((x = PEM_read_bio_X509(b, 0, 0, 0))) {
	    if (!SSL_CTX_add0_chain_cert(ctx, x)) {
		X509_free(x);
		BIO_free(b);
		return tls_fail(tls, err, err_len, "cannot add certificate from", ca);
	    }
	    n++;
	}
	BIO_free(b);
	/* the end of the file is reported as a missing start line */
	e = ERR_peek_last_error();
	if (!n || (e && !(ERR_GET_LIB(e) == ERR_LIB_PEM && ERR_GET_REASON(e) == PEM_R_NO_START_LINE)))
	    return tls_fail(tls, err, err_len, "cannot read certificates from", ca);
	ERR_clear_error();
    }

    if (client_ca) { /* client certificates are required */
	STACK_OF(X509_NAME) *names;
	if (SSL_CTX_load_verify_locations(ctx, client_ca, 0) != 1)
	    return tls_fail(tls, err, err_len, "cannot load client CA certificates from", client_ca);
	if (!(names = SSL_load_client_CA_file(client_ca)))
	    return tls_fail(tls, err, err_len, "cannot load client CA names from", client_ca);
	SSL_CTX_set_client_CA_list(ctx, names);
	SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, verify_cb);
	if (n_cn > 0) {
	    if (!(tls->cn = (char**) calloc(n_cn, sizeof(char*))))
		return tls_fail(tls, err, err_len, "cannot allocate TLS context", 0);
	    for (i = 0; i < n_cn; i++) {
		if (!(tls->cn[i] = strdup(cn[i])))
		    return tls_fail(tls, err, err_len, "cannot allocate TLS context", 0);
		tls->n_cn++;
	    }
	}
    }
    return tls;
}

/* sets the socket error as reported by sockerrno */
static void set_sock_error(int would_block) {
#ifdef WIN32
    WSASetLastError(would_block ? WSAEWOULDBLOCK : WSAECONNRESET);
#else
    errno = would_block ? EAGAIN : ECONNRESET;
#endif
}

/* translates the result of SSL_read/SSL_write to recv/send semantics */
static ssize_t tls_result(SSL *ssl, int n) {
    unsigned long e;
    char ebuf[256];
    if (n > 0)
	return n;
    switch (SSL_get_error(ssl, n)) {
    case SSL_ERROR_ZERO_RETURN: /* closed by the peer */
	return 0;
    case SSL_ERROR_WANT_READ:
    case SSL_ERROR_WANT_WRITE:
	set_sock_error(1);
	return -1;
    }
    /* fatal error: the connection cannot be used anymore, not even to
       send a shutdown alert */
    if ((e = ERR_peek_last_error())) {
	ERR_error_string_n(e, ebuf, sizeof(ebuf));
	ulog_info("TLS: connection error: %s", ebuf);
    }
    ERR_clear_error();
    SSL_set_quiet_shutdown(ssl, 1);
    set_sock_error(0);
    return -1;
}

static ssize_t tls_recv(client_connection_t *c, void *buf, size_t len) {
    SSL *ssl = (SSL*) c->info.ssl;
    ERR_clear_error();
    return tls_result(ssl, SSL_read(ssl, buf, (len > INT_MAX) ? INT_MAX : (int) len));
}

static ssize_t tls_send(client_connection_t *c, const void *buf, size_t len) {
    SSL *ssl = (SSL*) c->info.ssl;
    ERR_clear_error();
    return tls_result(ssl, SSL_write(ssl, buf, (len > INT_MAX) ? INT_MAX : (int) len));
}

void tls_server(server_t *srv) {
    srv->send = tls_send;
    srv->recv = tls_recv;
}

int add_tls(client_connection_t *c, tls_t *tls) {
    SSL *ssl = SSL_new(tls->ctx);
    if (!ssl)
	return -1;
    if (SSL_set_fd(ssl, (int) c->info.s) != 1) {
	SSL_free(ssl);
	ERR_clear_error();
	return -1;
    }
    SSL_set_accept_state(ssl);
    c->info.ssl = ssl;
    return 0;
}

int tls_pending(client_connection_t *c) {
    return (c->info.ssl && SSL_pending((SSL*) c->info.ssl) > 0);
}

void close_tls(client_connection_t *c) {
    if (c->info.ssl) {
	SSL *ssl = (SSL*) c->info.ssl;
	/* NOTE: we are not performing a two-step shutdown. */
	if (!SSL_in_init(ssl))
	    SSL_shutdown(ssl);
	SSL_free(ssl);
	ERR_clear_error();
	c->info.ssl = 0;
    }
}

#else /* no SSL/TLS support */

tls_t *new_tls(const char *cert, const char *key, const char *ca,
	       const char *client_ca, const char **cn, int n_cn,
	       char *err, size_t err_len) {
    snprintf(err, err_len, "TLS support is not available in this build");
    return 0;
}
void free_tls(tls_t *tls) { }
void tls_server(server_t *srv) { }
int  add_tls(client_connection_t *c, tls_t *tls) { return -1; }
void close_tls(client_connection_t *c) { }
int  tls_pending(client_connection_t *c) { return 0; }

#endif
