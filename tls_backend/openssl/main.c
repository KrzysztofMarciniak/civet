
#include "../../tls_interface.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#include <sys/types.h>
#include <sys/socket.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

struct openssl_context {
    SSL_CTX *ctx;
};

struct openssl_connection {
    int fd;
    SSL *ssl;
};

/*
 * OpenSSL errors are stored per thread so concurrent client threads
 * do not overwrite each other's diagnostic messages.
 */
#if defined(__GNUC__) || defined(__clang__)
static __thread char openssl_error_text[256];
#else
static char openssl_error_text[256];
#endif

static void
openssl_set_message(const char *message)
{
    (void)snprintf(openssl_error_text,
                   sizeof(openssl_error_text), "%s", message);
}

static void
openssl_set_error(const char *where)
{
    unsigned long code;
    char detail[160];

    code = ERR_get_error();

    if (code != 0) {
        ERR_error_string_n(code, detail, sizeof(detail));
        (void)snprintf(openssl_error_text,
                       sizeof(openssl_error_text),
                       "%s: %s", where, detail);
    } else {
        (void)snprintf(openssl_error_text,
                       sizeof(openssl_error_text),
                       "%s", where);
    }
}

static int
openssl_context_create(const struct tls_config *config, void **out)
{
    struct openssl_context *ctx;

    if (out == NULL) {
        openssl_set_message("Invalid TLS context output pointer");
        return -1;
    }
    *out = NULL;

    if (config == NULL ||
        config->certificate_file == NULL ||
        config->private_key_file == NULL) {
        openssl_set_message(
            "TLS certificate and private key are required");
        return -1;
    }

    if (OPENSSL_init_ssl(0, NULL) != 1) {
        openssl_set_error("OPENSSL_init_ssl failed");
        return -1;
    }

    ctx = (struct openssl_context *)calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        openssl_set_message("Out of memory creating TLS context");
        return -1;
    }

    ctx->ctx = SSL_CTX_new(TLS_server_method());
    if (ctx->ctx == NULL) {
        openssl_set_error("SSL_CTX_new failed");
        free(ctx);
        return -1;
    }

    if (SSL_CTX_set_min_proto_version(ctx->ctx,
                                      TLS1_2_VERSION) != 1) {
        openssl_set_error("Could not set minimum TLS version");
        SSL_CTX_free(ctx->ctx);
        free(ctx);
        return -1;
    }

    if (SSL_CTX_use_certificate_chain_file(
            ctx->ctx, config->certificate_file) != 1) {
        openssl_set_error("Could not load TLS certificate");
        SSL_CTX_free(ctx->ctx);
        free(ctx);
        return -1;
    }

    if (SSL_CTX_use_PrivateKey_file(
            ctx->ctx, config->private_key_file,
            SSL_FILETYPE_PEM) != 1) {
        openssl_set_error("Could not load TLS private key");
        SSL_CTX_free(ctx->ctx);
        free(ctx);
        return -1;
    }

    if (SSL_CTX_check_private_key(ctx->ctx) != 1) {
        openssl_set_error("TLS private key does not match certificate");
        SSL_CTX_free(ctx->ctx);
        free(ctx);
        return -1;
    }

    *out = ctx;
    openssl_error_text[0] = '\0';
    return 0;
}

static void
openssl_context_destroy(void *opaque)
{
    struct openssl_context *ctx;

    ctx = (struct openssl_context *)opaque;
    if (ctx == NULL)
        return;

    SSL_CTX_free(ctx->ctx);
    free(ctx);
}

static int
openssl_connection_accept(void *opaque, int fd, void **out)
{
    struct openssl_context *ctx;
    struct openssl_connection *conn;
    int ret;
    int ssl_error;

    if (out == NULL) {
        openssl_set_message("Invalid TLS connection output pointer");
        return -1;
    }
    *out = NULL;

    ctx = (struct openssl_context *)opaque;
    if (ctx == NULL || fd < 0) {
        openssl_set_message("Invalid TLS context or socket");
        return -1;
    }

    conn = (struct openssl_connection *)calloc(1, sizeof(*conn));
    if (conn == NULL) {
        openssl_set_message("Out of memory creating TLS connection");
        return -1;
    }

    conn->fd = fd;
    conn->ssl = SSL_new(ctx->ctx);
    if (conn->ssl == NULL) {
        openssl_set_error("SSL_new failed");
        free(conn);
        return -1;
    }

    if (SSL_set_fd(conn->ssl, fd) != 1) {
        openssl_set_error("SSL_set_fd failed");
        SSL_free(conn->ssl);
        free(conn);
        return -1;
    }

    for (;;) {
        ret = SSL_accept(conn->ssl);
        if (ret == 1)
            break;

        ssl_error = SSL_get_error(conn->ssl, ret);
        if (ssl_error == SSL_ERROR_WANT_READ ||
            ssl_error == SSL_ERROR_WANT_WRITE)
            continue;

        openssl_set_error("TLS handshake failed");
        SSL_free(conn->ssl);
        free(conn);
        return -1;
    }

    *out = conn;
    openssl_error_text[0] = '\0';
    return 0;
}

static ssize_t
openssl_connection_read(void *opaque, void *buffer, size_t length)
{
    struct openssl_connection *conn;
    int ret;
    int ssl_error;
    int chunk;

    conn = (struct openssl_connection *)opaque;
    if (conn == NULL || (buffer == NULL && length != 0)) {
        openssl_set_message("Invalid TLS read arguments");
        return -1;
    }

    if (length == 0)
        return 0;

    chunk = length > (size_t)INT_MAX ? INT_MAX : (int)length;

    for (;;) {
        ret = SSL_read(conn->ssl, buffer, chunk);
        if (ret > 0) {
            openssl_error_text[0] = '\0';
            return (ssize_t)ret;
        }

        ssl_error = SSL_get_error(conn->ssl, ret);

        if (ssl_error == SSL_ERROR_WANT_READ ||
            ssl_error == SSL_ERROR_WANT_WRITE)
            continue;

        if (ssl_error == SSL_ERROR_ZERO_RETURN)
            return 0;

        openssl_set_error("TLS read failed");
        return -1;
    }
}

static ssize_t
openssl_connection_write(void *opaque,
                         const void *buffer, size_t length)
{
    struct openssl_connection *conn;
    const unsigned char *p;
    size_t remaining;
    int chunk;
    int ret;
    int ssl_error;

    conn = (struct openssl_connection *)opaque;
    if (conn == NULL || (buffer == NULL && length != 0)) {
        openssl_set_message("Invalid TLS write arguments");
        return -1;
    }

    if (length == 0)
        return 0;

    p = (const unsigned char *)buffer;
    remaining = length;

    /*
     * Return the number of bytes written, matching ssize_t semantics.
     * A successful partial write is valid; the caller can retry.
     */
    chunk = remaining > (size_t)INT_MAX ? INT_MAX : (int)remaining;

    for (;;) {
        ret = SSL_write(conn->ssl, p, chunk);
        if (ret > 0) {
            openssl_error_text[0] = '\0';
            return (ssize_t)ret;
        }

        ssl_error = SSL_get_error(conn->ssl, ret);
        if (ssl_error == SSL_ERROR_WANT_READ ||
            ssl_error == SSL_ERROR_WANT_WRITE)
            continue;

        openssl_set_error("TLS write failed");
        return -1;
    }
}

static int
openssl_connection_shutdown(void *opaque)
{
    struct openssl_connection *conn;
    int ret;
    int ssl_error;

    conn = (struct openssl_connection *)opaque;
    if (conn == NULL) {
        openssl_set_message("Invalid TLS connection");
        return -1;
    }

    ret = SSL_shutdown(conn->ssl);
    if (ret == 1) {
        openssl_error_text[0] = '\0';
        return 0;
    }

    if (ret == 0) {
        /*
         * The close-notify alert was sent. Do not block waiting for the
         * peer's alert here; the socket may be closing.
         */
        openssl_error_text[0] = '\0';
        return 0;
    }

    ssl_error = SSL_get_error(conn->ssl, ret);
    if (ssl_error == SSL_ERROR_WANT_READ ||
        ssl_error == SSL_ERROR_WANT_WRITE) {
        openssl_set_message("TLS shutdown would block");
        return -1;
    }

    openssl_set_error("TLS shutdown failed");
    return -1;
}

static void
openssl_connection_destroy(void *opaque)
{
    struct openssl_connection *conn;

    conn = (struct openssl_connection *)opaque;
    if (conn == NULL)
        return;

    /*
     * The TCP descriptor belongs to the caller. SSL_free() does not
     * close it because SSL_set_fd() does not transfer ownership.
     */
    SSL_free(conn->ssl);
    free(conn);
}

static const char *
openssl_last_error(void)
{
    if (openssl_error_text[0] == '\0')
        return "no OpenSSL error available";

    return openssl_error_text;
}

static const struct tls_backend_ops openssl_ops = {
    "openssl",
    openssl_context_create,
    openssl_context_destroy,
    openssl_connection_accept,
    openssl_connection_read,
    openssl_connection_write,
    openssl_connection_shutdown,
    openssl_connection_destroy,
    openssl_last_error
};

const struct tls_backend_ops *
tls_backend_get(void)
{
    return &openssl_ops;
}
