#ifndef TLS_INTERFACE_H
#define TLS_INTERFACE_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Backend-independent TLS interface.
 *
 * Public API:
 *   tls_context_create()
 *   tls_context_destroy()
 *   tls_connection_accept()
 *   tls_connection_read()
 *   tls_connection_write()
 *   tls_connection_shutdown()
 *   tls_connection_destroy()
 *   tls_connection_fd()
 *   tls_last_error()
 *
 * A TLS context is shared between client threads.
 * Each client connection owns its own TLS session.
 */

struct tls_context;
struct tls_connection;

/*
 * TLS configuration.
 *
 * certificate_file and private_key_file may be NULL if the selected
 * backend obtains credentials through another configuration mechanism.
 *
 * backend_config is an optional backend-specific configuration string.
 * Its interpretation is defined by the selected backend.
 *
 * The interface does not take ownership of these strings.
 */
struct tls_config {
    const char *certificate_file;
    const char *private_key_file;
    const char *backend_config;
};

/*
 * Common return conventions:
 *
 *   0  success
 *  -1  failure
 *
 * Read/write:
 *  >0  number of bytes transferred
 *   0  orderly end of stream (read only)
 *  -1  failure
 *
 * Implementations must not report a successful write of zero bytes for
 * a nonzero requested length.
 */

/* Initialize the selected backend and create a shared TLS context. */
int tls_context_create(
    const struct tls_config *config,
    struct tls_context **out
);

/* Destroy the context after all its client connections are destroyed. */
void tls_context_destroy(struct tls_context *context);

/*
 * Create a per-client TLS connection and perform the server handshake.
 *
 * fd remains owned by the caller. Destroying a TLS connection does not
 * close fd.
 *
 * On failure, *out is NULL.
 */
int tls_connection_accept(
    struct tls_context *context,
    int fd,
    struct tls_connection **out
);

/* Read decrypted application data. */
ssize_t tls_connection_read(
    struct tls_connection *connection,
    void *buffer,
    size_t length
);

/* Write application data, encrypting it through TLS. */
ssize_t tls_connection_write(
    struct tls_connection *connection,
    const void *buffer,
    size_t length
);

/* Attempt a graceful TLS shutdown. */
int tls_connection_shutdown(struct tls_connection *connection);

/* Release the session and connection object; does not close the socket. */
void tls_connection_destroy(struct tls_connection *connection);

/* Return the underlying TCP descriptor, or -1 for an invalid connection. */
int tls_connection_fd(const struct tls_connection *connection);

/*
 * Return a human-readable error for the most recent TLS operation
 * on the calling thread. The returned pointer remains valid until the
 * next TLS operation on that thread.
 */
const char *tls_last_error(void);


/*
 * Backend implementer contract.
 *
 * The selected backend provides tls_backend_get().
 * The returned operations table must remain valid for the program lifetime.
 *
 * Backend context and session objects are owned by the backend.
 * Backend operations return 0 on success and -1 on failure, except read
 * and write, which use the public API's byte-count conventions.
 */

struct tls_backend_ops {
    const char *name;

    int (*context_create)(
        const struct tls_config *config,
        void **backend_context
    );

    void (*context_destroy)(void *backend_context);

    int (*connection_accept)(
        void *backend_context,
        int fd,
        void **backend_session
    );

    ssize_t (*connection_read)(
        void *backend_session,
        void *buffer,
        size_t length
    );

    ssize_t (*connection_write)(
        void *backend_session,
        const void *buffer,
        size_t length
    );

    int (*connection_shutdown)(void *backend_session);

    void (*connection_destroy)(void *backend_session);

    const char *(*last_error)(void);
};

/* Implemented by the selected TLS backend. */
const struct tls_backend_ops *tls_backend_get(void);

#ifdef __cplusplus
}
#endif

#endif /* TLS_INTERFACE_H */
