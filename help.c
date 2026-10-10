#include "help.h"

#include <stdio.h>

void usage(const char *prog)
{
#ifdef ENABLE_TLS
    fprintf(stderr,
            "usage: %s [-b address] [-p port] -r root [-c paths]"
            " [-C cert] [-K key]\n"
            "\n"
            "  -b address   bind address (default: 127.0.0.1)\n"
            "  -p port      listen port (default: 8080)\n"
            "  -r root      document root\n"
            "  -c paths     comma-separated cache paths\n"
            "  -C cert      TLS certificate file (PEM)\n"
            "  -K key       TLS private key file (PEM)\n",
            prog);
#else
    fprintf(stderr,
            "usage: %s [-b address] [-p port] -r root [-c paths]\n"
            "\n"
            "  -b address   bind address (default: 127.0.0.1)\n"
            "  -p port      listen port (default: 8080)\n"
            "  -r root      document root\n"
            "  -c paths     comma-separated cache paths\n",
            prog);
#endif
}
