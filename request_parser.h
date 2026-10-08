#ifndef REQUEST_PARSER_H
#define REQUEST_PARSER_H

#include <stddef.h>

#include "lib.h"

#define REQUEST_MAX_HEADERS 64
#define REQUEST_MAX_TARGET 4096
#define REQUEST_MAX_HEADER_NAME 256
#define REQUEST_MAX_HEADER_VALUE 8192
#define REQUEST_MAX_HEADER_BYTES 16384

#define REQUEST_METHOD_GET 1
#define REQUEST_METHOD_HEAD 2
#define REQUEST_METHOD_POST 3
#define REQUEST_METHOD_PUT 4
#define REQUEST_METHOD_PATCH 5
#define REQUEST_METHOD_DELETE 6

#define REQUEST_HTTP_10 10
#define REQUEST_HTTP_11 11

#define REQUEST_PARSE_OK 0
#define REQUEST_PARSE_INCOMPLETE 1
#define REQUEST_PARSE_BAD (-1)
#define REQUEST_PARSE_TOO_LARGE (-2)

struct request_header {
        const char* name;
        size_t name_len;

        const char* value;
        size_t value_len;
};

struct http_request {
        s4 method;
        s4 version;

        char target[REQUEST_MAX_TARGET];

        struct request_header headers[REQUEST_MAX_HEADERS];
        s4 header_count;

        s4 has_content_length;
        u8 content_length;

        s4 has_transfer_encoding;

        size_t header_bytes;
};

s4 request_parse(const char* buf, size_t len, struct http_request* req,
                 size_t* consumed);

const struct request_header* request_header_get(const struct http_request* req,
                                                const char* name);

#endif
