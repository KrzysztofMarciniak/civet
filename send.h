#ifndef SEND_H
#define SEND_H

#include <stddef.h>
#include "lib.h"

#ifdef ENABLE_TLS
#include "tls_interface.h"
#endif

struct client_transport {
        int fd;
#ifdef ENABLE_TLS
        struct tls_connection *tls_conn;
#endif
};

/*
 * Send up to len bytes.
 *
 * Returns:
 *   > 0  Number of bytes sent.
 *   -1   Error.
 */
s4 client_send(struct client_transport *transport,
               const void *buf, size_t len);

/*
 * Send all len bytes unless an error occurs.
 *
 * Returns:
 *    0  Success.
 *   -1  Error.
 */
s4 client_send_all(struct client_transport *transport,
                   const void *buf, size_t len);

#endif /* SEND_H */
