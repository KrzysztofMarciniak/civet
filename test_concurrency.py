#!/usr/bin/env python3

import socket
import sys
import time

HOST = "127.0.0.1"
PORT = 8080
MAX_THREADS = 64

sockets = []

print("Opening 64 incomplete connections...")

try:
    for i in range(MAX_THREADS):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((HOST, PORT))
        s.sendall(b"GET / HTTP/1.1\r\n")
        sockets.append(s)

    print("64 connections open.")
    time.sleep(1)

    print("Opening connection 65...")

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(2)
    s.connect((HOST, PORT))
    s.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")

    response = s.recv(1024).decode("ascii", "replace")
    first_line = response.splitlines()[0] if response else "<empty>"

    print("Response:", first_line)

    if response.startswith("HTTP/1.1 503 "):
        print("PASS: SERVER_MAX_THREADS limit is enforced.")
        sys.exit(0)

    print("FAIL: expected HTTP/1.1 503 Service Unavailable.")
    sys.exit(1)

except Exception as e:
    print("FAIL:", e)
    sys.exit(1)

finally:
    for s in sockets:
        try:
            s.close()
        except Exception:
            pass
