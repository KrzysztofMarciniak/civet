#!/usr/bin/env sh

set -eu

ROOT="."

find "$ROOT" -type f \( \
    -name '*.c' -o \
    -name '*.h' -o \
    -name '*.cc' -o \
    -name '*.cpp' -o \
    -name '*.cxx' \
\) -exec clang-format -i {} +

printf '%s\n' "==> Formatted source files"
