#include "server_threads.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "debug.h"
#include "request_parser.h"

#define SERVER_REQUEST_BUF 16384
#define SERVER_FILE_BUF 8192
#define SERVER_SOCKET_TIMEOUT_SECS 10
#define SERVER_REQUEST_TIMEOUT_SECS 10

static pthread_mutex_t thread_mutex = PTHREAD_MUTEX_INITIALIZER;
static s4 thread_count              = 0;

static s4 set_socket_timeouts(int fd) {
	struct timeval tv;

	debug_log("thread", "set_socket_timeouts: ENTER");

	tv.tv_sec  = SERVER_SOCKET_TIMEOUT_SECS;
	tv.tv_usec = 0;

	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
		debug_log("thread", "set_socket_timeouts: SO_RCVTIMEO failed");
		return -1;
	}

	if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
		debug_log("thread", "set_socket_timeouts: SO_SNDTIMEO failed");
		return -1;
	}

	debug_log("thread", "set_socket_timeouts: EXIT");
	return 0;
}

static s4 send_all(int fd, const char* buf, size_t len) {
	ssize_t n;

	debug_log("thread", "send_all: ENTER");

	while (len > 0) {
		n = send(fd, buf, len, 0);

		if (n < 0) {
			if (errno == EINTR) continue;

			debug_log("thread", "send_all: send failed");
			return -1;
		}

		if (n == 0) {
			debug_log("thread", "send_all: send returned zero");
			return -1;
		}

		buf += n;
		len -= (size_t)n;
	}

	debug_log("thread", "send_all: EXIT");

	return 0;
}

static s4 send_response(int fd, int status, const char* reason,
                        const char* type, const char* body) {
	char buf[1024];
	int n;
	size_t len;

	debug_log("thread", "send_response: ENTER");

	len = strlen(body);

	n = snprintf(buf, sizeof(buf),
	             "HTTP/1.1 %d %s\r\n"
	             "Content-Type: %s\r\n"
	             "Content-Length: %lu\r\n"
	             "Connection: close\r\n"
	             "\r\n",
	             status, reason, type, (unsigned long)len);

	if (n < 0 || (size_t)n >= sizeof(buf)) return -1;

	if (send_all(fd, buf, (size_t)n) < 0) return -1;

	return send_all(fd, body, len);
}

static const char* mime_type(const char* path) {
	const char* dot;

	dot = strrchr(path, '.');

	if (dot == NULL) return "application/octet-stream";

	if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0)
		return "text/html";

	if (strcmp(dot, ".css") == 0) return "text/css";

	if (strcmp(dot, ".js") == 0) return "application/javascript";

	if (strcmp(dot, ".json") == 0) return "application/json";

	if (strcmp(dot, ".txt") == 0) return "text/plain";

	if (strcmp(dot, ".xml") == 0) return "application/xml";

	if (strcmp(dot, ".svg") == 0) return "image/svg+xml";

	if (strcmp(dot, ".png") == 0) return "image/png";

	if (strcmp(dot, ".gif") == 0) return "image/gif";

	if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0)
		return "image/jpeg";

	if (strcmp(dot, ".webp") == 0) return "image/webp";

	if (strcmp(dot, ".ico") == 0) return "image/x-icon";

	if (strcmp(dot, ".pdf") == 0) return "application/pdf";

	return "application/octet-stream";
}

static s4 send_file_headers(int fd, const char* path, off_t size) {
	char buf[1024];
	int n;

	debug_log("thread", "send_file_headers: ENTER");

	n = snprintf(buf, sizeof(buf),
	             "HTTP/1.1 200 OK\r\n"
	             "Content-Type: %s\r\n"
	             "Content-Length: %lu\r\n"
	             "Connection: close\r\n"
	             "\r\n",
	             mime_type(path), (unsigned long)size);

	if (n < 0 || (size_t)n >= sizeof(buf)) return -1;

	return send_all(fd, buf, (size_t)n);
}

static s4 send_cached_file(int fd, const struct cache_entry* entry,
                          s4 head_only) {
	char buf[1024];
	int n;

	debug_log("thread", "send_cached_file: ENTER");

	n = snprintf(buf, sizeof(buf),
	             "HTTP/1.1 200 OK\r\n"
	             "Content-Type: %s\r\n"
	             "Content-Length: %lu\r\n"
	             "Connection: close\r\n"
	             "\r\n",
	             mime_type(entry->url_path), (unsigned long)entry->size);

	if (n < 0 || (size_t)n >= sizeof(buf)) return -1;

	if (send_all(fd, buf, (size_t)n) < 0) return -1;

	if (head_only) return 0;

	return send_all(fd, entry->content, entry->size);
}

static s4 send_file(int fd, int file_fd, const char* url_path, s4 head_only) {
	struct stat st;
	char buf[SERVER_FILE_BUF];
	ssize_t n;

	debug_log("thread", "send_file: ENTER");

	if (fstat(file_fd, &st) < 0) {
		debug_log("thread", "send_file: fstat failed");
		close(file_fd);
		return -1;
	}

	if (!S_ISREG(st.st_mode)) {
		debug_log("thread", "send_file: not regular");
		close(file_fd);
		return -1;
	}

	if (send_file_headers(fd, url_path, st.st_size) < 0) {
		debug_log("thread", "send_file: headers failed");
		close(file_fd);
		return -1;
	}

	if (head_only) {
		close(file_fd);
		return 0;
	}

	for (;;) {
		n = read(file_fd, buf, sizeof(buf));

		if (n < 0) {
			if (errno == EINTR) continue;

			debug_log("thread", "send_file: read failed");
			close(file_fd);
			return -1;
		}

		if (n == 0) break;

		if (send_all(fd, buf, (size_t)n) < 0) {
			debug_log("thread", "send_file: send failed");
			close(file_fd);
			return -1;
		}
	}

	close(file_fd);

	debug_log("thread", "send_file: EXIT");

	return 0;
}

static const struct vfs_entry* find_index(const struct vfs* vfs,
                                          const char* path) {
	char index_path[REQUEST_MAX_TARGET];
	size_t len;
	const struct vfs_entry* entry;

	debug_log("thread", "find_index: ENTER");

	if (strcmp(path, "/") == 0) {
		entry = vfs_lookup(vfs, "/index.html");

		debug_log("thread", "find_index: root lookup complete");

		if (entry != NULL && entry->type == VFS_FILE) return entry;

		return NULL;
	}

	len = strlen(path);

	if (len + sizeof("/index.html") > sizeof(index_path)) return NULL;

	memcpy(index_path, path, len);

	if (index_path[len - 1] != '/') index_path[len++] = '/';

	memcpy(index_path + len, "index.html", sizeof("index.html"));

	entry = vfs_lookup(vfs, index_path);

	if (entry != NULL && entry->type == VFS_FILE) return entry;

	return NULL;
}

static s4 handle_request(int fd, struct vfs* vfs, struct vfs_server* vfs_server,
                         struct cache* cache, const struct http_request* req) {
	char path[REQUEST_MAX_TARGET];
	size_t len;
	const struct vfs_entry* entry;
	const struct cache_entry* cached;
	int file_fd;

	debug_log("thread", "handle_request: ENTER");

	debug_log("thread", "handle_request: BEFORE normalize");

	if (vfs_normalize_path(req->target, path, sizeof(path)) != VFS_OK) {
		debug_log("thread", "handle_request: normalize failed");

		return send_response(fd, 400, "Bad Request", "text/plain",
		                     "bad request\n");
	}

	debug_log("thread", "handle_request: AFTER normalize");

	len = strlen(path);

	while (len > 1 && path[len - 1] == '/') path[--len] = '\0';

	debug_log("thread", "handle_request: BEFORE vfs_lookup");

	entry = vfs_lookup(vfs, path);

	debug_log("thread", "handle_request: AFTER vfs_lookup");

	if (entry == NULL) {
		debug_log("thread", "handle_request: not found");

		return send_response(fd, 404, "Not Found", "text/plain",
		                     "not found\n");
	}

	debug_log("thread", "handle_request: entry found");

	if (entry->type == VFS_DIRECTORY) {
		debug_log("thread", "handle_request: directory");

		entry = find_index(vfs, path);

		debug_log("thread", "handle_request: AFTER find_index");

		if (entry == NULL) {
			return send_response(fd, 404, "Not Found", "text/plain",
			                     "not found\n");
		}
	}

	if (entry->type != VFS_FILE)
		return send_response(fd, 404, "Not Found", "text/plain",
		                     "not found\n");

	debug_log("thread", "handle_request: BEFORE cache_lookup");

	cached = cache_lookup(cache, entry->url_path);

	debug_log("thread", "handle_request: AFTER cache_lookup");

	if (cached != NULL) {
		debug_log("thread", "handle_request: cache HIT");

		return send_cached_file(fd, cached,
		                         req->method == REQUEST_METHOD_HEAD);
	}

	debug_log("thread", "handle_request: cache MISS");
	debug_log("thread", "handle_request: BEFORE vfs_server_open");

	file_fd = vfs_server_open(vfs_server, entry);

	debug_log("thread", "handle_request: AFTER vfs_server_open");

	if (file_fd < 0) {
		if (errno == ENOENT || errno == ENOTDIR || errno == ELOOP ||
		    errno == EACCES) {
			return send_response(fd, 404, "Not Found", "text/plain",
			                     "not found\n");
		}

		return send_response(fd, 500, "Internal Server Error",
		                     "text/plain", "internal server error\n");
	}

	debug_log("thread", "handle_request: BEFORE send_file");

	return send_file(fd, file_fd, entry->url_path,
	                  req->method == REQUEST_METHOD_HEAD);
}

static s4 handle_client(int fd, struct vfs* vfs, struct vfs_server* vfs_server,
                        struct cache* cache) {
	char buf[SERVER_REQUEST_BUF];
	struct http_request* req;
	size_t used;
	size_t consumed;
	ssize_t n;
	s4 rc;
	time_t deadline;

	debug_log("thread", "handle_client: ENTER");

	req = (struct http_request*)malloc(sizeof(*req));

	if (req == NULL) {
		debug_log("thread", "handle_client: malloc failed");
		return -1;
	}

	used = 0;
	deadline = time(NULL) + SERVER_REQUEST_TIMEOUT_SECS;

	debug_log("thread", "handle_client: BEFORE recv");

	for (;;) {
		if (time(NULL) >= deadline) {
			debug_log("thread", "handle_client: request deadline exceeded");
			free(req);
			return -1;
		}

		n = recv(fd, buf + used, sizeof(buf) - used, 0);

		debug_log("thread", "handle_client: AFTER recv");

		if (n < 0) {
			if (errno == EINTR) continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				debug_log("thread", "handle_client: recv timeout");
				free(req);
				return -1;
			}

			debug_log("thread", "handle_client: recv failed");
			free(req);
			return -1;
		}

		if (n == 0) {
			debug_log("thread", "handle_client: peer closed");
			free(req);
			return 0;
		}

		used += (size_t)n;

		debug_log("thread", "handle_client: BEFORE request_parse");

		rc = request_parse(buf, used, req, &consumed);

		debug_log("thread", "handle_client: AFTER request_parse");

		if (rc == REQUEST_PARSE_INCOMPLETE) {
			if (used == sizeof(buf)) {
				send_response(
					fd, 431, "Request Header Fields Too Large",
					"text/plain", "request too large\n");
				free(req);
				return -1;
			}

			continue;
		}

		if (rc == REQUEST_PARSE_TOO_LARGE) {
			send_response(fd, 431,
			              "Request Header Fields Too Large",
			              "text/plain", "request too large\n");
			free(req);
			return -1;
		}

		if (rc != REQUEST_PARSE_OK) {
			send_response(fd, 400, "Bad Request", "text/plain",
			              "bad request\n");
			free(req);
			return -1;
		}

		break;
	}

	(void)consumed;

	debug_log("thread", "handle_client: BEFORE handle_request");

	rc = handle_request(fd, vfs, vfs_server, cache, req);

	debug_log("thread", "handle_client: AFTER handle_request");

	free(req);

	return rc;
}

static void* server_thread_main(void* arg) {
	struct server_thread_args* args;
	int client_fd;
	struct vfs* vfs;
	struct vfs_server* vfs_server;
	struct cache* cache;

	debug_log("thread", "server_thread_main: ENTER");

	args = (struct server_thread_args*)arg;

	debug_log("thread", "server_thread_main: args assigned");

	client_fd = args->client_fd;
	debug_log("thread", "server_thread_main: client_fd assigned");

	vfs = args->vfs;
	debug_log("thread", "server_thread_main: vfs assigned");

	vfs_server = args->vfs_server;
	debug_log("thread", "server_thread_main: vfs_server assigned");

	cache = args->cache;
	debug_log("thread", "server_thread_main: cache assigned");

	debug_log("thread", "server_thread_main: BEFORE set_socket_timeouts");

	if (set_socket_timeouts(client_fd) < 0) {
		debug_log("thread", "server_thread_main: set_socket_timeouts failed");
		close(client_fd);

		pthread_mutex_lock(&thread_mutex);
		thread_count--;
		pthread_mutex_unlock(&thread_mutex);

		free(args);
		return NULL;
	}

	debug_log("thread", "server_thread_main: AFTER set_socket_timeouts");

	debug_log("thread", "server_thread_main: BEFORE handle_client");

	handle_client(client_fd, vfs, vfs_server, cache);

	debug_log("thread", "server_thread_main: AFTER handle_client");

	close(client_fd);

	pthread_mutex_lock(&thread_mutex);
	thread_count--;
	pthread_mutex_unlock(&thread_mutex);

	debug_log("thread", "server_thread_main: BEFORE free");

	free(args);

	debug_log("thread", "server_thread_main: EXIT");

	return NULL;
}

s4 server_thread_start(int client_fd, struct vfs* vfs,
                       struct vfs_server* vfs_server, struct cache* cache) {
	pthread_t thread;
	pthread_attr_t attr;
	struct server_thread_args* args;
	int rc;

	debug_log("thread", "server_thread_start: ENTER");

	pthread_mutex_lock(&thread_mutex);

	if (thread_count >= SERVER_MAX_THREADS) {
		pthread_mutex_unlock(&thread_mutex);
		debug_log("thread", "server_thread_start: thread limit reached");
		return -1;
	}

	thread_count++;

	pthread_mutex_unlock(&thread_mutex);

	args = (struct server_thread_args*)malloc(sizeof(*args));

	debug_log("thread", "server_thread_start: AFTER malloc");

	if (args == NULL) {
		pthread_mutex_lock(&thread_mutex);
		thread_count--;
		pthread_mutex_unlock(&thread_mutex);
		return -1;
	}

	args->client_fd  = client_fd;
	args->vfs        = vfs;
	args->vfs_server = vfs_server;
	args->cache      = cache;

	debug_log("thread", "server_thread_start: args initialized");

	rc = pthread_attr_init(&attr);

	debug_log("thread", "server_thread_start: AFTER pthread_attr_init");

	if (rc != 0) {
		pthread_mutex_lock(&thread_mutex);
		thread_count--;
		pthread_mutex_unlock(&thread_mutex);

		free(args);
		return -1;
	}

	rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	debug_log("thread", "server_thread_start: AFTER pthread_attr_setdetachstate");

	if (rc != 0) {
		pthread_attr_destroy(&attr);

		pthread_mutex_lock(&thread_mutex);
		thread_count--;
		pthread_mutex_unlock(&thread_mutex);

		free(args);
		return -1;
	}

	debug_log("thread", "server_thread_start: BEFORE pthread_create");

	rc = pthread_create(&thread, &attr, server_thread_main, args);

	debug_log("thread", "server_thread_start: AFTER pthread_create");

	pthread_attr_destroy(&attr);

	if (rc != 0) {
		pthread_mutex_lock(&thread_mutex);
		thread_count--;
		pthread_mutex_unlock(&thread_mutex);

		free(args);
		return -1;
	}

	debug_log("thread", "server_thread_start: SUCCESS");

	return 0;
}
