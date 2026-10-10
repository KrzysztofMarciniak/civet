#!/usr/bin/env sh

set -eu

OUT_DIR="${1:-certs}"
DAYS="${DAYS:-1}"
KEY="$OUT_DIR/server.key"
CERT="$OUT_DIR/server.crt"

if ! command -v openssl >/dev/null 2>&1; then
    echo "Error: openssl is required to generate the test certificate." >&2
    exit 1
fi

mkdir -p "$OUT_DIR"

if [ -e "$KEY" ] || [ -e "$CERT" ]; then
    echo "Refusing to overwrite an existing key or certificate in $OUT_DIR." >&2
    echo "Remove them first if you want to regenerate." >&2
    exit 1
fi

openssl req -x509 -newkey rsa:2048 -sha256 -nodes \
    -keyout "$KEY" \
    -out "$CERT" \
    -days "$DAYS" \
    -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"

chmod 600 "$KEY"
chmod 644 "$CERT"

echo "Generated test TLS files:"
echo "  Certificate: $CERT"
echo "  Private key: $KEY"
