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

# make DEBUG=1   runtime debug logging + -g
DEBUG ?= 0

ifeq ($(DEBUG),1)
DEFS   += -DDEBUG
CFLAGS += -g
endif

SRCS = main.c allowed_chars.c port.c request_parser.c \
       vfs.c vfs_server.c cache.c server.c server_threads.c
OBJS = $(SRCS:.c=.o)
DEPS = $(OBJS:.o=.d)

.SUFFIXES:
.DELETE_ON_ERROR:
.PHONY: all install uninstall clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) $(THREADS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(DEFS) $(THREADS) $(DEPFLAGS) -c -o $@ $<

-include $(DEPS)

install: $(TARGET)
	mkdir -p $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	$(STRIP) -s $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)
