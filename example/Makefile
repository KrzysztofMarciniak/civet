CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

CGI_DIR = website/cgi-bin
CGI_SCRIPTS = $(CGI_DIR)/hello.bin

all: $(CGI_SCRIPTS)

$(CGI_DIR)/hello.bin: hello.c
	@mkdir -p $(CGI_DIR)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(CGI_SCRIPTS) *.o

.PHONY: all clean
