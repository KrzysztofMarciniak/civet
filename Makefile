TARGET   = civet

CFLAGS  ?= -O2
WARN     = -std=c89 -pedantic -Wall -Wextra
DEFS     = -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700
THREADS  = -pthread
DEPFLAGS = -MMD -MP

STRIP   ?= strip
PREFIX  ?= /usr/local
BINDIR   = $(PREFIX)/bin
DESTDIR ?=

# Optional TLS configuration:
#   make tls=1 mbed=1
#   make tls=1 openssl=1
#   make tls=1 libressl=1
tls     ?= 0
mbed    ?= 0
openssl ?= 0
libressl ?= 0

DEBUG ?= 0

ifeq ($(DEBUG),1)
DEFS   += -DDEBUG
CFLAGS += -g
endif

SRCS = main.c allowed_chars.c port.c request_parser.c \
       vfs.c hash.c vfs_server.c cgi-bin.c cache.c server.c \
       server_threads.c help.c debug.c send.c

TLS_CPPFLAGS =
TLS_LDLIBS   =
TLS_SRCS     =

# TLS backend selection and validation.
ifeq ($(tls),1)

ifneq ($(words $(filter 1,$(mbed) $(openssl) $(libressl))),1)
$(error With tls=1, select exactly one backend: mbed=1, openssl=1, or libressl=1)
endif

SRCS += tls_interface.c
DEFS += -DENABLE_TLS

ifeq ($(mbed),1)
TLS_CPPFLAGS += $(shell pkg-config --cflags mbedtls 2>/dev/null)
TLS_LDLIBS   += $(shell pkg-config --libs mbedtls 2>/dev/null)
ifeq ($(strip $(TLS_LDLIBS)),)
TLS_LDLIBS = -lmbedtls -lmbedx509 -lmbedcrypto
endif
TLS_SRCS += tls_backend/mbed/main.c
DEFS += -DTLS_BACKEND_MBED

else ifeq ($(openssl),1)
TLS_CPPFLAGS += $(shell pkg-config --cflags openssl 2>/dev/null)
TLS_LDLIBS   += $(shell pkg-config --libs openssl 2>/dev/null)
ifeq ($(strip $(TLS_LDLIBS)),)
TLS_LDLIBS = -lssl -lcrypto
endif
TLS_SRCS += tls_backend/openssl/main.c
DEFS += -DTLS_BACKEND_OPENSSL

else ifeq ($(libressl),1)
TLS_CPPFLAGS += $(shell pkg-config --cflags libressl 2>/dev/null)
TLS_LDLIBS   += $(shell pkg-config --libs libressl 2>/dev/null)
ifeq ($(strip $(TLS_LDLIBS)),)
TLS_LDLIBS = -lssl -lcrypto
endif
TLS_SRCS += tls_backend/libressl/main.c
DEFS += -DTLS_BACKEND_LIBRESSL
endif

SRCS += $(TLS_SRCS)

else

ifneq ($(filter 1,$(mbed) $(openssl) $(libressl)),)
$(error TLS backend selected without tls=1; use make tls=1 <backend>=1)
endif

endif

CPPFLAGS += $(TLS_CPPFLAGS)
LDLIBS   += $(TLS_LDLIBS)

OBJS = $(SRCS:.c=.o)
DEPS = $(OBJS:.o=.d)

.SUFFIXES:
.DELETE_ON_ERROR:
.PHONY: all install uninstall clean test test-http test-https

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) $(THREADS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(DEFS) $(THREADS) $(DEPFLAGS) -c -o $@ $<

-include $(DEPS)

# Tests live in test/ and are driven by test/run.sh:
#   make test        HTTP suite, then HTTPS suite (both always run)
#   make test-http   HTTP suite only (plain civet on :8080)
#   make test-https  HTTPS suite only (TLS civet on :8443)
#
# The HTTPS suite uses a separate OpenSSL-enabled build in build/tls/ so the
# plaintext objects in the source tree are left untouched. Self-signed test
# certificates are written to certs/ (gitignored).
PYTHON  ?= python3

TLS_BUILD_DIR = build/tls
TLS_BIN       = $(TLS_BUILD_DIR)/$(TARGET)
TLS_INPUTS    = $(wildcard *.c *.h) $(wildcard tls_backend/*/*.c tls_backend/*/*.h) Makefile

$(TLS_BIN): $(TLS_INPUTS)
	mkdir -p $(TLS_BUILD_DIR)
	cp -R $(wildcard *.c *.h) Makefile tls_backend $(TLS_BUILD_DIR)/
	$(MAKE) -C $(TLS_BUILD_DIR) tls=1 openssl=1

TEST_ENV = PYTHON="$(PYTHON)" BIN="$(CURDIR)/$(TARGET)" TLS_BIN="$(CURDIR)/$(TLS_BIN)"

test: $(TARGET) $(TLS_BIN)
	$(TEST_ENV) sh test/run.sh all

test-http: $(TARGET)
	$(TEST_ENV) sh test/run.sh http

test-https: $(TLS_BIN)
	$(TEST_ENV) sh test/run.sh https

install: $(TARGET)
	mkdir -p $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	$(STRIP) -s $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)
	rm -rf $(TLS_BUILD_DIR)
