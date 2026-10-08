#ifndef CIVET_CGI_BIN_H
#define CIVET_CGI_BIN_H

#include <stddef.h>

#include "request_parser.h"
#include "vfs.h"

#define CGI_MAX_ENV_VARS   128
#define CGI_MAX_ENV_BYTES  32768UL
#define CGI_PREFIX     "/cgi-bin/"
#define CGI_TIMEOUT    10
#define CGI_MAX_OUTPUT (1024UL * 1024UL)
#define CGI_MAX_BODY   65536UL

struct cgi_request {
        const char* method;
        const char* query;

        const char* content_type;

        const char* remote_addr;
        const char* server_name;
        const char* server_port;

        const char* body;
        size_t body_len;

        /*
         * Points directly at the headers parsed by request_parser.
         * Header names and values are length-delimited.
         */
        const struct request_header* headers;
        size_t header_count;
};

s4 cgi_match(const struct vfs* vfs, const char* url_path,
             const struct vfs_entry** out);

s4 cgi_run(const struct vfs* vfs, const struct vfs_entry* entry,
           const struct cgi_request* req, int out_fd);

#endif
