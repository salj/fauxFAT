#!/bin/sh
set -eu

: "${ARM_CC:=clang}"
: "${NM:=nm}"
: "${LD_LLD:=ld.lld}"
: "${LIB:=build/m0plus/libxxh32_m0plus.a}"
: "${GENERIC_LIB:=build/generic/libxxh32_generic.a}"
: "${HOST_CC:=cc}"

out=${TMPDIR:-/tmp}/xxh32_archive.$$
trap 'rm -rf "$out"' EXIT HUP INT TERM
mkdir -p "$out"

cflags='-std=c11 -O2 -ffreestanding -Iinclude -target thumbv6m-none-eabi -mcpu=cortex-m0plus -mthumb'

cat > "$out/oneshot.c" <<'SRC'
#include <xxhash.h>
XXH32_hash_t probe(const void *p, size_t n) { return XXH32(p, n, 0); }
SRC
$HOST_CC -std=c11 -O2 -Iinclude -c "$out/oneshot.c" -o "$out/oneshot-host.o"
$LD_LLD -r "$out/oneshot-host.o" "$GENERIC_LIB" -o "$out/oneshot-generic.link.o"
$NM "$out/oneshot-generic.link.o" | grep -q ' T XXH32$'
for sym in XXH32_update XXH32_createState XXH32_canonicalFromHash XXH_versionNumber malloc free memcpy; do
    if $NM "$out/oneshot-generic.link.o" | grep -q " $sym$"; then
        echo "$sym leaked into generic one-shot link" >&2
        exit 1
    fi
done

$ARM_CC $cflags -c "$out/oneshot.c" -o "$out/oneshot.o"
$LD_LLD -r "$out/oneshot.o" "$LIB" -o "$out/oneshot.link.o"
$NM "$out/oneshot.link.o" | grep -q ' T XXH32$'
$NM "$out/oneshot.link.o" | grep -q ' T xxh32_m0plus_oneshot_aligned$'
for sym in XXH32_update XXH32_createState XXH32_canonicalFromHash XXH_versionNumber xxh32_m0plus_rounds_aligned malloc free memcpy; do
    if $NM "$out/oneshot.link.o" | grep -q " $sym$"; then
        echo "$sym leaked into one-shot link" >&2
        exit 1
    fi
done

cat > "$out/stream.c" <<'SRC'
#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>
XXH32_hash_t probe(const void *p, size_t n) {
    XXH32_state_t s;
    XXH32_reset(&s, 0);
    XXH32_update(&s, p, n);
    return XXH32_digest(&s);
}
SRC
$ARM_CC $cflags -c "$out/stream.c" -o "$out/stream.o"
$LD_LLD -r "$out/stream.o" "$LIB" -o "$out/stream.link.o"
$NM "$out/stream.link.o" | grep -q ' T XXH32_update$'
$NM "$out/stream.link.o" | grep -q ' T xxh32_m0plus_rounds_aligned$'
for sym in 'XXH32$' XXH32_createState XXH32_canonicalFromHash XXH_versionNumber xxh32_m0plus_oneshot_aligned malloc free memcpy; do
    if $NM "$out/stream.link.o" | grep -E -q " T $sym| U $sym"; then
        echo "$sym leaked into streaming link" >&2
        exit 1
    fi
done

printf 'archive member tests OK\n'
