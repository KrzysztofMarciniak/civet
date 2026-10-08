#ifndef CGI_H
#define CGI_H

#include "lib.h"
#include "request_parser.h"
#include "vfs.h"

s4 cgi_execute(int client_fd, const struct vfs_entry* entry,
               const struct http_request* req, const char* vfs_root);

#endif
