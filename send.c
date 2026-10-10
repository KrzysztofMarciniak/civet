#include "send.h"

#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/socket.h>

s4 client_send(struct client_transport *transport,
               const void *buf, size_t len)
{
        size_t amount;
        ssize_t n;

        if (transport == NULL || (buf == NULL && len != 0)) {
                errno = EINVAL;
                return -1;
        }

        if (len == 0)
                return 0;

#ifdef ENABLE_TLS
        if (transport->tls_conn != NULL) {
                /*
                 * The TLS interface must guarantee that its return
                 * value fits in s4 and follows the same error contract.
                 */
                return (s4)tls_connection_write(
                        transport->tls_conn, buf,
                        len > (size_t)S4_MAX
                                ? (size_t)S4_MAX : len);
        }
#endif

        amount = len;
        if (amount > (size_t)S4_MAX)
                amount = (size_t)S4_MAX;

        n = send(transport->fd, buf, amount, 0);
        if (n < 0)
                return -1;

        /*
         * amount <= S4_MAX, so a successful result fits in s4.
         */
        return (s4)n;
}

s4 client_send_all(struct client_transport *transport,
                   const void *buf, size_t len)
{
        const unsigned char *p;
        size_t sent;
        s4 n;

        if (transport == NULL || (buf == NULL && len != 0)) {
                errno = EINVAL;
                return -1;
        }

        p = (const unsigned char *)buf;
        sent = 0;

        while (sent < len) {
                n = client_send(transport, p + sent, len - sent);

                if (n < 0) {
                        if (errno == EINTR)
                                continue;
                        return -1;
                }

                if (n == 0) {
                        errno = EPIPE;
                        return -1;
                }

                sent += (size_t)n;
        }

        return 0;
}
