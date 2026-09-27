#!/bin/sh
set -eu
: "${CC:=cc}"
out=${TMPDIR:-/tmp}/xxh32-poison.$$
trap 'rm -rf "$out"' EXIT HUP INT TERM
mkdir -p "$out"
cat > "$out/bad.c" <<'SRC'
#include <xxhash.h>
int main(void) { return (int)XXH64("x", 1, 0); }
SRC
if "$CC" -std=c11 -Iinclude -c "$out/bad.c" -o "$out/bad.o" >"$out/log" 2>&1; then
    echo 'unsupported XXH64 unexpectedly compiled' >&2
    exit 1
fi
grep -Eqi 'poison|unsupported|XXH64' "$out/log"
printf 'unsupported API poison test OK\n'
