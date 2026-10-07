# civet

<p align="center">
  <img src="logo.png" alt="civet logo" width="160">
</p>

<p align="center">
  Lean, small HTTP static file server written in C, with an in-memory virtual filesystem.
</p>

## Build

```sh
make
```

Install:

```sh
make install
```

## Debug build:

```sh
make DEBUG=1
```

This enables runtime debug logging.

## Quick start

Serve `./test` on `127.0.0.1:8080` and cache `index.html` and `other_page.html` in memory:

```sh
civet -r ./test -p 8080 -b 127.0.0.1 -c index.html,other_page.html
```

The equivalent configuration uses:

* `-r ./test` — document root
* `-p 8080` — HTTP port
* `-b 127.0.0.1` — bind address
* `-c index.html,other_page.html` — preload selected files into memory

Cached files are loaded into RAM when Civet starts and served directly from the cache.

Example startup output:

```text
civet: indexed 3 filesystem entries
civet: cached /index.html (1234 bytes)
civet: cached /other_page.html (2345 bytes)
civet: cache contains 2 files (3579 bytes)
civet: virtual filesystem:
/
|-- index.html
|-- other_page.html
civet: 1 directories, 2 files
civet: serving /path/to/test on http://127.0.0.1:8080/
civet: listening on 127.0.0.1:8080
civet: serving /path/to/test
```

## Usage

```text
Usage: civet [options] [DIRECTORY]

Serve the files in DIRECTORY over HTTP (GET and HEAD only).
DIRECTORY defaults to the current directory.

Options:
  -r, --root DIR    directory to serve (same as DIRECTORY)
  -p, --port N      TCP port to listen on     (default 8080)
  -b, --bind ADDR   IPv4 address to listen on (default 127.0.0.1)
                    use 0.0.0.0 to accept connections from anywhere
  -c, --cache PATHS comma-separated files to cache in memory
                    e.g. -c index.html,css/style.css
  -h, --help        show this help and exit
  -v, --version     show version and exit
```

## Features

* Small POSIX HTTP server
* HTTP/1.0 and HTTP/1.1 request parsing
* `GET` and `HEAD` support
* In-memory virtual filesystem index
* Optional in-memory file cache
* Directory `index.html` support
* MIME type detection
* Configurable bind address and port
* Static file serving only

## Requirements

* POSIX system
* C compiler
* `make`



## Size

(`version 0.0.1`)
Built on x86-64 with musl:

```text
binary:     115K
text:       24,416 bytes
data:          960 bytes
bss:           456 bytes
total:      25,832 bytes (25.2 KiB)
```
For comparison, [quark](https://git.suckless.org/quark/) built from commit
5ad0df91757fbc577ffceeca633725e962da345d:

```
binary:     42K
text:       29,154 bytes
data:        6,000 bytes
bss:         4,208 bytes
total:      39,362 bytes (38.4 KiB)
```

Civet's text/data/bss footprint is ~34% smaller.

## License

See the project source for license information.
