#!/usr/bin/env python3

import argparse
import hashlib
import socket
import ssl
import sys
import urllib.error
import urllib.request

URL_DEFAULT = "https://localhost:8080"


def make_context(verify, ca_file, min_tls):
    if ca_file:
        ctx = ssl.create_default_context(cafile=ca_file)
    elif verify:
        ctx = ssl.create_default_context()
    else:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE

    if min_tls == "1.2":
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    elif min_tls == "1.3":
        ctx.minimum_version = ssl.TLSVersion.TLSv1_3

    return ctx


def check_tls(host, port, context, timeout):
    print("\n[1] TLS handshake and encryption")

    with socket.create_connection((host, port), timeout=timeout) as raw:
        raw.settimeout(timeout)
        with context.wrap_socket(raw, server_hostname=host) as tls:
            print("  PASS: TLS handshake succeeded")
            print("  TLS version:", tls.version())

            cipher = tls.cipher()
            if cipher:
                print("  Cipher:", cipher[0])
                print("  Cipher protocol:", cipher[1])
                print("  Cipher bits:", cipher[2])
                if cipher[2] >= 128:
                    print("  PASS: cipher uses at least 128-bit encryption")
                else:
                    print("  FAIL: cipher has less than 128-bit encryption")
            else:
                print("  FAIL: no negotiated cipher reported")

            cert_bin = tls.getpeercert(binary_form=True)
            if not cert_bin:
                print("  FAIL: server did not present a certificate")
                return False

            print("  Certificate SHA-256:",
                  hashlib.sha256(cert_bin).hexdigest())

            cert = tls.getpeercert()
            if cert:
                print("  Subject:", cert.get("subject", "(unavailable)"))
                print("  Issuer:", cert.get("issuer", "(unavailable)"))
                print("  Valid from:", cert.get("notBefore", "(unavailable)"))
                print("  Valid until:", cert.get("notAfter", "(unavailable)"))
            else:
                print("  Certificate details unavailable because verification "
                      "is disabled; fingerprint is still available.")

            print("  PASS: application traffic is carried inside TLS")
            return True


def check_https_request(url, context, timeout):
    print("\n[2] HTTP response over HTTPS")

    req = urllib.request.Request(
        url,
        headers={
            "User-Agent": "civet-https-test/1.0",
            "Connection": "close",
        },
    )

    try:
        response = urllib.request.urlopen(
            req, context=context, timeout=timeout
        )
    except urllib.error.HTTPError as exc:
        response = exc
    except Exception as exc:
        print("  FAIL:", exc)
        return False

    try:
        body = response.read(2 * 1024 * 1024)
        print("  HTTP status:", response.status)
        print("  Final URL:", response.geturl())
        print("  Content-Type:", response.headers.get("Content-Type", "(none)"))
        print("  Content-Length:", response.headers.get("Content-Length", "(none)"))
        print("  Bytes received:", len(body))

        if response.geturl().lower().startswith("https://"):
            print("  PASS: request remained on HTTPS")
        else:
            print("  WARNING: request ended at a non-HTTPS URL")

        print("  PASS: received an HTTP response through TLS")
        return True
    finally:
        response.close()


def check_plaintext_rejected(host, port, timeout):
    print("\n[3] Plaintext HTTP against HTTPS port")
    request = (
        "GET / HTTP/1.1\r\n"
        "Host: {}\r\n"
        "Connection: close\r\n\r\n".format(host)
    ).encode("ascii")

    try:
        with socket.create_connection((host, port), timeout=timeout) as sock:
            sock.settimeout(timeout)
            sock.sendall(request)
            try:
                data = sock.recv(4096)
            except (socket.timeout, ConnectionResetError, OSError):
                data = b""

        if data.startswith(b"HTTP/"):
            print("  FAIL: server returned an HTTP response to plaintext input")
            print("  Response:", repr(data[:200]))
            return False

        print("  PASS: no plaintext HTTP response returned")
        if data:
            print("  Received non-HTTP bytes:", repr(data[:80]))
        else:
            print("  Server closed/reset the plaintext connection")
        return True
    except (ConnectionResetError, socket.timeout, OSError) as exc:
        print("  PASS: plaintext connection was rejected:", exc)
        return True


def check_repeated_connections(host, port, context, timeout, count):
    print("\n[4] Repeated TLS handshakes")
    failures = 0

    for i in range(count):
        try:
            with socket.create_connection(
                (host, port), timeout=timeout
            ) as raw:
                raw.settimeout(timeout)
                with context.wrap_socket(raw, server_hostname=host) as tls:
                    if not tls.version() or not tls.cipher():
                        raise RuntimeError("TLS version or cipher missing")
            print("  PASS: connection {}/{}".format(i + 1, count))
        except Exception as exc:
            failures += 1
            print("  FAIL: connection {}/{}: {}".format(i + 1, count, exc))

    return failures == 0


def main():
    parser = argparse.ArgumentParser(
        description="TLS security smoke test for civet"
    )
    parser.add_argument("--host", default="localhost")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--path", default="/")
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--repeat", type=int, default=5)
    parser.add_argument("--verify", action="store_true",
                        help="verify certificate against system trust store")
    parser.add_argument("--ca-file",
                        help="CA certificate PEM file for certificate verification")
    parser.add_argument("--min-tls", choices=("default", "1.2", "1.3"),
                        default="default",
                        help="require this minimum TLS version")
    args = parser.parse_args()

    if args.port < 1 or args.port > 65535:
        parser.error("port must be between 1 and 65535")
    if args.timeout <= 0 or args.repeat < 1:
        parser.error("timeout must be positive and repeat must be at least 1")
    if not args.path.startswith("/"):
        parser.error("path must begin with /")

    url = "https://{}:{}{}".format(args.host, args.port, args.path)
    context = make_context(args.verify, args.ca_file, args.min_tls)

    print("civet HTTPS/TLS test")
    print("Target:", url)
    print("Certificate verification:",
          "enabled" if args.verify or args.ca_file else "DISABLED (self-signed test mode)")
    if not args.verify and not args.ca_file:
        print("WARNING: encryption is tested, but certificate identity/trust is NOT.")

    results = []
    try:
        results.append(("TLS encryption", check_tls(
            args.host, args.port, context, args.timeout
        )))
    except Exception as exc:
        print("\n[1] FAIL:", exc)
        results.append(("TLS encryption", False))
        print("\nCannot run HTTPS tests without a working TLS connection.")
        return 1

    results.append(("HTTPS request", check_https_request(
        url, context, args.timeout
    )))
    results.append(("Plaintext rejection", check_plaintext_rejected(
        args.host, args.port, args.timeout
    )))
    results.append(("Repeated TLS connections", check_repeated_connections(
        args.host, args.port, context, args.timeout, args.repeat
    )))

    print("\nSummary")
    for name, passed in results:
        print("  {}: {}".format("PASS" if passed else "FAIL", name))

    failed = sum(1 for _, passed in results if not passed)
    print("\n{} passed; {} failed".format(len(results) - failed, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
