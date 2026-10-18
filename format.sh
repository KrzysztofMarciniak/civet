#!/usr/bin/env sh

set -eu

ROOT=$(CDPATH= cd "$(dirname "$0")" && pwd)
CLANG_FORMAT=${CLANG_FORMAT:-clang-format}
MODE=format

if [ "$#" -gt 1 ]; then
    printf '%s\n' "Usage: $0 [--check]" >&2
    exit 2
fi

if [ "$#" -eq 1 ]; then
    if [ "$1" != "--check" ]; then
        printf '%s\n' "Usage: $0 [--check]" >&2
        exit 2
    fi
    MODE=check
fi

if ! command -v "$CLANG_FORMAT" >/dev/null 2>&1; then
    printf 'clang-format executable not found: %s\n' "$CLANG_FORMAT" >&2
    exit 127
fi

cd "$ROOT"

format_sources() {
    find . -type d \( -name .git -o -path ./build/tls \) -prune -o \
        -type f \( \
            -name '*.c' -o \
            -name '*.h' -o \
            -name '*.cc' -o \
            -name '*.cpp' -o \
            -name '*.cxx' \
        \) -exec "$CLANG_FORMAT" "$@" {} +
}

if [ "$MODE" = check ]; then
    format_sources --dry-run --Werror
    printf '%s\n' "==> Source formatting is clean"
    exit 0
fi

format_sources -i

printf '%s\n' "==> Formatted source files"
