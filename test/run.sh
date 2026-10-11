#!/usr/bin/env sh
#
# civet smoke test runner.
#
# Usage: sh test/run.sh [http|https|all]      (default: all)
#
#   http   plain-text server on 127.0.0.1:8080
#          runs test_attacks.py and test_concurrency.py
#   https  TLS server on 127.0.0.1:8443 using a self-signed certificate
#          in certs/ (generated if missing or expiring); runs test_https.py
#
# Environment overrides:
#   PYTHON   python interpreter        (default: python3)
#   BIN      plain civet binary        (default: ./civet)
#   TLS_BIN  TLS-enabled civet binary  (default: ./build/tls/civet)

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PYTHON=${PYTHON:-python3}
BIN=${BIN:-$ROOT/civet}
TLS_BIN=${TLS_BIN:-$ROOT/build/tls/civet}
DOCROOT=$ROOT/example/website
CERT_DIR=$ROOT/certs
CERT=$CERT_DIR/server.crt
KEY=$CERT_DIR/server.key
HTTP_PORT=8080
HTTPS_PORT=8443
MODE=${1:-all}

case $MODE in
    http|https|all) ;;
    *)
        echo "usage: $0 [http|https|all]" >&2
        exit 2
        ;;
esac

if [ -t 1 ]; then
    C_PASS=$(printf '\033[1;32m')
    C_FAIL=$(printf '\033[1;31m')
    C_HEAD=$(printf '\033[1;36m')
    C_DIM=$(printf '\033[2m')
    C_OFF=$(printf '\033[0m')
else
    C_PASS=''
    C_FAIL=''
    C_HEAD=''
    C_DIM=''
    C_OFF=''
fi

LOG=$(mktemp) || exit 1
SERVER_PID=
SUITE_PASS=0
SUITE_TOTAL=0
SUMMARY=''
OVERALL=0

stop_server() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
        SERVER_PID=
    fi
}

cleanup() {
    stop_server
    rm -f "$LOG"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

heading() {
    printf '\n%s== %s ==%s\n' "$C_HEAD" "$1" "$C_OFF"
}

reset_suite() {
    SUITE_PASS=0
    SUITE_TOTAL=0
}

# Runs a command as one test step and reports PASS/FAIL with its duration.
run_step() {
    label=$1
    shift
    printf '\n%s>> %s%s\n' "$C_DIM" "$label" "$C_OFF"
    t0=$(date +%s)
    SUITE_TOTAL=$((SUITE_TOTAL + 1))
    if "$@"; then
        SUITE_PASS=$((SUITE_PASS + 1))
        status=PASS
        color=$C_PASS
    else
        status=FAIL
        color=$C_FAIL
        OVERALL=1
    fi
    printf '%s[%s]%s %s (%ss)\n' "$color" "$status" "$C_OFF" "$label" \
        "$(( $(date +%s) - t0 ))"
}

record_failure() {
    SUITE_TOTAL=$((SUITE_TOTAL + 1))
    OVERALL=1
    printf '%s[FAIL]%s %s\n' "$C_FAIL" "$C_OFF" "$1"
}

finish_suite() {
    if [ "$SUITE_PASS" -eq "$SUITE_TOTAL" ]; then
        tag="${C_PASS}PASS${C_OFF}"
    else
        tag="${C_FAIL}FAIL${C_OFF}"
    fi
    SUMMARY="${SUMMARY}
  $1  $tag  $SUITE_PASS/$SUITE_TOTAL steps passed"
}

port_open() {
    "$PYTHON" -c 'import socket, sys
socket.create_connection(("127.0.0.1", int(sys.argv[1])), 1).close()' \
        "$1" 2>/dev/null
}

# start_server BIN PORT [extra civet options...]
start_server() {
    bin=$1
    port=$2
    shift 2

    "$bin" -p "$port" -r "$DOCROOT" "$@" >"$LOG" 2>&1 &
    SERVER_PID=$!

    i=0
    while ! port_open "$port"; do
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            echo "civet exited during startup:" >&2
            sed 's/^/    /' "$LOG" >&2
            SERVER_PID=
            return 1
        fi
        i=$((i + 1))
        if [ "$i" -ge 50 ]; then
            echo "civet did not listen on port $port within 5s" >&2
            return 1
        fi
        sleep 0.1
    done
    return 0
}

# Quiet wrapper around gen_test_tls.sh; openssl's keygen progress goes to LOG
# and is only shown if generation fails.
gen_certs() {
    if DAYS=30 sh "$ROOT/test/gen_test_tls.sh" "$CERT_DIR" >/dev/null 2>"$LOG"; then
        return 0
    fi
    sed 's/^/    /' "$LOG" >&2
    return 1
}

# Generates a self-signed certificate unless a usable one already exists.
ensure_certs() {
    if [ -f "$CERT" ] && [ -f "$KEY" ] &&
        openssl x509 -in "$CERT" -checkend 3600 -noout >/dev/null 2>&1; then
        printf '%s(using existing certificate in certs/)%s\n' "$C_DIM" "$C_OFF"
        return 0
    fi
    rm -f "$CERT" "$KEY"
    run_step "generate self-signed test certificate (30 days)" gen_certs
}

run_http() {
    heading "HTTP  (plain, 127.0.0.1:$HTTP_PORT)"
    reset_suite

    if start_server "$BIN" "$HTTP_PORT"; then
        run_step "attack smoke tests (test_attacks.py)" \
            "$PYTHON" "$ROOT/test/test_attacks.py"
        run_step "concurrency limit (test_concurrency.py)" \
            "$PYTHON" "$ROOT/test/test_concurrency.py"
    else
        record_failure "server failed to start on port $HTTP_PORT"
    fi
    stop_server

    finish_suite HTTP
}

run_https() {
    heading "HTTPS (TLS, 127.0.0.1:$HTTPS_PORT)"
    reset_suite

    if ensure_certs; then
        if start_server "$TLS_BIN" "$HTTPS_PORT" -C "$CERT" -K "$KEY"; then
            run_step "TLS and HTTPS checks (test_https.py)" \
                "$PYTHON" "$ROOT/test/test_https.py" \
                --port "$HTTPS_PORT" --ca-file "$CERT" --min-tls 1.2
        else
            record_failure "TLS server failed to start on port $HTTPS_PORT"
        fi
        stop_server
    fi

    finish_suite HTTPS
}

case $MODE in
    http)  run_http ;;
    https) run_https ;;
    all)
        run_http
        run_https
        ;;
esac

heading "Summary"
printf '%s\n' "$SUMMARY"
printf '\n'
if [ "$OVERALL" -eq 0 ]; then
    printf '%sAll tests passed.%s\n' "$C_PASS" "$C_OFF"
else
    printf '%sSome tests failed.%s\n' "$C_FAIL" "$C_OFF"
fi

exit "$OVERALL"
