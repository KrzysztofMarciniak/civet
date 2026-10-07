CC      = cc
CFLAGS  = -O2 -g
WARN    = -std=c89 -pedantic -Wall -Wextra
DEFS    = -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700
THREADS = -pthread

ifeq ($(DEBUG),1)
DEFS += -DDEBUG
endif

TARGET  = civet

PREFIX  = /usr/local
BINDIR  = $(PREFIX)/bin
DESTDIR =

OBJS    = main.o \
          allowed_chars.o \
          port.o \
          request_parser.o \
          vfs.o \
          vfs_server.o \
          cache.o \
          server.o \
          server_threads.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(THREADS) $(LDFLAGS) -o $@ $(OBJS)

.c.o:
	$(CC) $(CFLAGS) $(WARN) $(DEFS) $(THREADS) -c $<

main.o: main.c \
        lib.h \
        port.h \
        allowed_chars.h \
        vfs.h \
        vfs_server.h \
        cache.h \
        server.h

allowed_chars.o: allowed_chars.c \
                 allowed_chars.h \
                 lib.h

port.o: port.c \
        port.h \
        lib.h

request_parser.o: request_parser.c \
                  request_parser.h \
                  allowed_chars.h \
                  lib.h

vfs.o: vfs.c \
       vfs.h \
       lib.h

vfs_server.o: vfs_server.c \
              vfs_server.h \
              vfs.h \
              lib.h

cache.o: cache.c \
         cache.h \
         vfs.h \
         vfs_server.h \
         lib.h

server.o: server.c \
          server.h \
          server_threads.h \
          vfs.h \
          vfs_server.h \
          cache.h \
          lib.h

server_threads.o: server_threads.c \
                  server_threads.h \
                  request_parser.h \
                  vfs.h \
                  vfs_server.h \
                  cache.h \
                  lib.h

install: $(TARGET)
	mkdir -p $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all install uninstall clean
