#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cc_bin=${CC:-cc}
output="${TMPDIR:-/tmp}/wasm3-direct-api-test"

"$cc_bin" -std=gnu99 -O2 -I"$repo_root/source" \
    "$repo_root/test/direct/test_direct.c" \
    "$repo_root"/source/*.c \
    -lm -o "$output"

"$output" "$repo_root/test/lang/fib32.wasm"
