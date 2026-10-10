#include "tls_interface.h"

#include <limits.h>
#include <stdlib.h>

struct tls_context {
        const struct tls_backend_ops* ops;
        void* backend_context;
};

struct tls_connection {
        struct tls_context* context;
        void* backend_session;
        int fd;
};

static _Thread_local const char* interface_error;

static void set_interface_error(const char* message) {
        interface_error = message;
}

static int backend_ops_valid(const struct tls_backend_ops* ops) {
        if (ops == NULL) return 0;
        if (ops->name == NULL) return 0;
        if (ops->context_create == NULL) return 0;
        if (ops->context_destroy == NULL) return 0;
        if (ops->connection_accept == NULL) return 0;
        if (ops->connection_read == NULL) return 0;
        if (ops->connection_write == NULL) return 0;
        if (ops->connection_shutdown == NULL) return 0;
        if (ops->connection_destroy == NULL) return 0;
        if (ops->last_error == NULL) return 0;

        return 1;
}

int tls_context_create(const struct tls_config* config,
                       struct tls_context** out) {
        struct tls_context* context;
        const struct tls_backend_ops* ops;

        set_interface_error(NULL);

        if (out == NULL) {
                set_interface_error("invalid output pointer");
                return -1;
        }

        *out = NULL;

        if (config == NULL) {
                set_interface_error("invalid TLS configuration");
                return -1;
        }

        ops = tls_backend_get();

        if (!backend_ops_valid(ops)) {
                set_interface_error("invalid TLS backend");
                return -1;
        }

        context = (struct tls_context*)calloc(1, sizeof(*context));

        if (context == NULL) {
                set_interface_error("failed to allocate TLS context");
                return -1;
        }

        context->ops = ops;

        if (ops->context_create(config, &context->backend_context) < 0) {
                if (context->backend_context != NULL) {
                        ops->context_destroy(context->backend_context);
                }

                free(context);
                return -1;
        }

        *out = context;
        return 0;
}

void tls_context_destroy(struct tls_context* context) {
        if (context == NULL) return;

        if (context->backend_context != NULL) {
                context->ops->context_destroy(context->backend_context);
                context->backend_context = NULL;
        }

        free(context);
}

int tls_connection_accept(struct tls_context* context, int fd,
                          struct tls_connection** out) {
        struct tls_connection* connection;

        set_interface_error(NULL);

        if (out == NULL) {
                set_interface_error("invalid output pointer");
                return -1;
        }

        *out = NULL;

        if (context == NULL || fd < 0) {
                set_interface_error("invalid TLS connection arguments");
                return -1;
        }

        connection = (struct tls_connection*)calloc(1, sizeof(*connection));

        if (connection == NULL) {
                set_interface_error("failed to allocate TLS connection");
                return -1;
        }

        connection->context = context;
        connection->fd      = fd;

        if (context->ops->connection_accept(context->backend_context, fd,
                                            &connection->backend_session) < 0) {
                if (connection->backend_session != NULL) {
                        context->ops->connection_destroy(
                            connection->backend_session);
                }

                free(connection);
                return -1;
        }

        *out = connection;
        return 0;
}

ssize_t tls_connection_read(struct tls_connection* connection, void* buffer,
                            size_t length) {
        ssize_t result;

        set_interface_error(NULL);

        if (connection == NULL || (buffer == NULL && length != 0)) {
                set_interface_error("invalid TLS read arguments");
                return -1;
        }

        if (length == 0) return 0;

        result = connection->context->ops->connection_read(
            connection->backend_session, buffer, length);

        if (result > 0 && (size_t)result > length) {
                set_interface_error(
                    "TLS backend returned an invalid read length");
                return -1;
        }

        return result;
}

ssize_t tls_connection_write(struct tls_connection* connection,
                             const void* buffer, size_t length) {
        ssize_t result;

        set_interface_error(NULL);

        if (connection == NULL || (buffer == NULL && length != 0)) {
                set_interface_error("invalid TLS write arguments");
                return -1;
        }

        if (length == 0) return 0;

        if (length > (size_t)SSIZE_MAX) {
                set_interface_error("TLS write length exceeds SSIZE_MAX");
                return -1;
        }

        result = connection->context->ops->connection_write(
            connection->backend_session, buffer, length);

        if (result > 0 && (size_t)result > length) {
                set_interface_error(
                    "TLS backend returned an invalid write length");
                return -1;
        }

        return result;
}

int tls_connection_shutdown(struct tls_connection* connection) {
        set_interface_error(NULL);

        if (connection == NULL) {
                set_interface_error("invalid TLS connection");
                return -1;
        }

        return connection->context->ops->connection_shutdown(
            connection->backend_session);
}

void tls_connection_destroy(struct tls_connection* connection) {
        if (connection == NULL) return;

        connection->context->ops->connection_destroy(
            connection->backend_session);

        connection->backend_session = NULL;
        free(connection);
}

int tls_connection_fd(const struct tls_connection* connection) {
        if (connection == NULL) return -1;

        return connection->fd;
}

const char* tls_last_error(void) {
        const char* message;

        if (interface_error != NULL) {
                return interface_error;
        }

        /*
         * Backend errors should be kept thread-local by the backend.
         */
        message = NULL;

        /*
         * The public connection/context objects are opaque, so backend
         * error retrieval for a failed context creation is handled directly
         * by the backend's own diagnostics.
         *
         * Connection-specific errors are retrieved through the backend
         * error callback in the operation wrappers in a future extension.
         */
        (void)message;

        return "no TLS interface error available";
}
