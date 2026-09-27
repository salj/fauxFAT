#!/bin/sh
set -eu
: "${CC:=cc}"
: "${NM:=nm}"
out=${TMPDIR:-/tmp}/xxh32-nostream.$$
trap 'rm -rf "$out"' EXIT HUP INT TERM
mkdir -p "$out"
common='-std=c11 -O2 -Wall -Wextra -Werror -DXXH_NO_STREAM -Iinclude -Isrc'
for src in xxh32_oneshot xxh32_stream xxh32_copy xxh32_alloc xxh32_canonical xxh32_version; do
    $CC $common -c "src/$src.c" -o "$out/$src.o"
done
if $NM "$out"/*.o | grep -E ' XXH32_(reset|update|digest|copyState|createState|freeState)$'; then
    echo 'streaming symbol present with XXH_NO_STREAM' >&2
    exit 1
fi
cat > "$out/main.c" <<'SRC'
#include <xxhash.h>
int main(void) { return XXH32("abc", 3, 0) == 0x32d153ffU ? 0 : 1; }
SRC
$CC $common "$out/main.c" "$out/xxh32_oneshot.o" -o "$out/main"
"$out/main"
printf 'XXH_NO_STREAM build test OK\n'
