# Makefile for CGI test scripts

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

CGI_SCRIPTS = hello.cgi

all: $(CGI_SCRIPTS)

hello.cgi: hello.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(CGI_SCRIPTS) *.o

.PHONY: all clean
