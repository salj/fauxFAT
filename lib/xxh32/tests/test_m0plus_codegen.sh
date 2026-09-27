#!/bin/sh
set -eu

: "${ARM_CC:=clang}"
: "${NM:=nm}"
: "${OBJDUMP:=llvm-objdump}"

out=${TMPDIR:-/tmp}/xxh32_m0plus_codegen.$$
trap 'rm -rf "$out"' EXIT HUP INT TERM
mkdir -p "$out"

cflags='-std=c11 -O3 -ffreestanding -fomit-frame-pointer -Wall -Wextra -Werror -DXXH32_BACKEND_M0PLUS=1 -Iinclude -Isrc -target thumbv6m-none-eabi -mcpu=cortex-m0plus -mthumb'

$ARM_CC $cflags -c src/xxh32_oneshot.c -o "$out/oneshot.o"
$ARM_CC $cflags -c src/xxh32_stream.c -o "$out/stream.o"

$NM -u "$out/oneshot.o" | grep -q 'xxh32_m0plus_oneshot_aligned'
$NM -u "$out/stream.o" | grep -q 'xxh32_m0plus_rounds_aligned'

$ARM_CC -target thumbv6m-none-eabi -mcpu=cortex-m0plus -mthumb \
    -c src/m0plus/xxh32_oneshot.S -o "$out/oneshot_asm.o"
$ARM_CC -target thumbv6m-none-eabi -mcpu=cortex-m0plus -mthumb \
    -c src/m0plus/xxh32_stream.S -o "$out/stream_asm.o"

$OBJDUMP -d "$out/oneshot_asm.o" > "$out/oneshot.dis"
$OBJDUMP -d "$out/stream_asm.o" > "$out/stream.dis"

grep -q '<xxh32_m0plus_oneshot_aligned>:' "$out/oneshot.dis"
grep -q '<xxh32_m0plus_rounds_aligned>:' "$out/stream.dis"
grep -q 'muls' "$out/oneshot.dis"
grep -q 'rors' "$out/oneshot.dis"
grep -q 'muls' "$out/stream.dis"
grep -q 'rors' "$out/stream.dis"

printf 'm0plus codegen tests OK\n'
