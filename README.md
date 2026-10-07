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

## Virtual filesystem and security

Civet uses an in-memory virtual filesystem (VFS) as a boundary between HTTP request paths and the real filesystem.

At startup, Civet scans the configured document root and builds an index of the files and directories that are actually available to the server. HTTP requests are then resolved against this virtual namespace rather than directly concatenated with the document-root path.

This design helps prevent common path traversal problems such as:

```text
/../../etc/passwd
```

and encoded variants such as:

```text
/%2e%2e/%2e%2e/etc/passwd
```

Request paths are normalized before lookup. Percent-encoded paths are decoded, `..` components are rejected, and the final path must exist in the VFS.

The VFS also allows Civet to ignore symbolic links and unsupported filesystem objects during indexing. This keeps the server's HTTP namespace explicit instead of allowing arbitrary filesystem paths to become reachable through URL manipulation.

The VFS is therefore both a performance-oriented index and an important security boundary.

**Security note:** Civet is intentionally a small static file server, not a hardened production web server. Run it with the minimum filesystem permissions required, avoid serving sensitive directories, and review the configuration before binding it to a public network interface.

## Features

* Small POSIX HTTP server
* HTTP/1.0 and HTTP/1.1 request parsing
* `GET` and `HEAD` support
* In-memory virtual filesystem index
* Path normalization and traversal protection
* Optional in-memory file cache
* Directory `index.html` support
* MIME type detection
* Configurable bind address and port
* Static file serving only

## Requirements

* POSIX system
* C compiler
* `make`

## License

See the project source for license information.
