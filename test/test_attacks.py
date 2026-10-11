#!/usr/bin/env python3

import socket
import sys

HOST = "127.0.0.1"
PORT = 8080
TIMEOUT = 2.0


def request(raw):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(TIMEOUT)

    try:
        sock.connect((HOST, PORT))
        sock.sendall(raw)

        data = b""

        while len(data) < 65536:
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                break

            if not chunk:
                break

            data += chunk

        return data

    finally:
        sock.close()


def status(response):
    if not response:
        return None

    line = response.split(b"\r\n", 1)[0]

    try:
        return int(line.split(b" ", 2)[1])
    except (IndexError, ValueError):
        return None


def run(name, raw, expect_ok):
    try:
        response = request(raw)
    except OSError as exc:
        print("FAIL  %-28s %s" % (name, exc))
        return False

    code = status(response)

    if expect_ok:
        passed = code == 200
    else:
        passed = code is not None and code != 200

    if passed:
        print("PASS  %-28s HTTP %s" % (name, code))
    else:
        print("FAIL  %-28s HTTP %s" % (name, code))

    return passed


def main():
    tests = [
        (
            "normal request",
            b"GET / HTTP/1.1\r\n"
            b"Host: localhost\r\n"
            b"\r\n",
            True,
        ),
        (
            "path traversal",
            b"GET /../../../../etc/passwd HTTP/1.1\r\n"
            b"Host: localhost\r\n"
            b"\r\n",
            False,
        ),
        (
            "encoded traversal",
            b"GET /%2e%2e/%2e%2e/etc/passwd HTTP/1.1\r\n"
            b"Host: localhost\r\n"
            b"\r\n",
            False,
        ),
        (
            "double encoded traversal",
            b"GET /%252e%252e/%252e%252e/etc/passwd HTTP/1.1\r\n"
            b"Host: localhost\r\n"
            b"\r\n",
            False,
        ),
        (
            "bad percent escape",
            b"GET /%ZZ HTTP/1.1\r\n"
            b"Host: localhost\r\n"
            b"\r\n",
            False,
        ),
        (
            "unsupported method",
            b"POST / HTTP/1.1\r\n"
            b"Host: localhost\r\n"
            b"\r\n",
            False,
        ),
        (
            "duplicate content-length",
            b"GET / HTTP/1.1\r\n"
            b"Host: localhost\r\n"
            b"Content-Length: 0\r\n"
            b"Content-Length: 0\r\n"
            b"\r\n",
            False,
        ),
        (
            "transfer-encoding",
            b"GET / HTTP/1.1\r\n"
            b"Host: localhost\r\n"
            b"Transfer-Encoding: chunked\r\n"
            b"\r\n",
            False,
        ),
        (
            "missing host",
            b"GET / HTTP/1.1\r\n"
            b"\r\n",
            False,
        ),
        (
            "duplicate host",
            b"GET / HTTP/1.1\r\n"
            b"Host: localhost\r\n"
            b"Host: localhost\r\n"
            b"\r\n",
            False,
        ),
        (
            "whitespace before colon",
            b"GET / HTTP/1.1\r\n"
            b"Host : localhost\r\n"
            b"\r\n",
            False,
        ),
        (
            "folded header",
            b"GET / HTTP/1.1\r\n"
            b"Host: localhost\r\n"
            b" X-Test: bad\r\n"
            b"\r\n",
            False,
        ),
        (
            "control in header",
            b"GET / HTTP/1.1\r\n"
            b"Host: local\x00host\r\n"
            b"\r\n",
            False,
        ),
        (
            "oversized target",
            b"GET /" + (b"A" * 5000) +
            b" HTTP/1.1\r\n"
            b"Host: localhost\r\n"
            b"\r\n",
            False,
        ),
        (
            "oversized header",
            b"GET / HTTP/1.1\r\n"
            b"Host: localhost\r\n"
            b"X-Test: " + (b"A" * 9000) +
            b"\r\n\r\n",
            False,
        ),
        (
            "http 0.9 style request",
            b"GET /\r\n",
            False,
        ),
    ]

    passed = 0

    print("civet attack smoke test")
    print("target: %s:%d" % (HOST, PORT))
    print()

    for name, raw, expect_ok in tests:
        if run(name, raw, expect_ok):
            passed += 1

    print()
    print("%d/%d tests passed" % (passed, len(tests)))

    if passed != len(tests):
        print("FAIL")
        return 1

    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
