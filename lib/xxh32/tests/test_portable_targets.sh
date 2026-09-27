#!/bin/sh
set -eu

: "${ARM_CC:=clang}"
: "${RISCV_CC:=clang}"
: "${NM:=nm}"

out=${TMPDIR:-/tmp}/xxh32_targets.$$
trap 'rm -rf "$out"' EXIT HUP INT TERM
mkdir -p "$out"

common='-std=c11 -O3 -ffreestanding -fomit-frame-pointer -Wall -Wextra -Werror -Iinclude -Isrc'

for src in xxh32_oneshot xxh32_stream xxh32_copy; do
    $ARM_CC $common -target thumbv8m.main-none-eabi -mcpu=cortex-m33 -mthumb \
        -c "src/$src.c" -o "$out/m33-$src.o"
    $RISCV_CC $common -target riscv32-none-elf \
        -march=rv32ima_zicsr_zifencei_zba_zbb_zbs_zbkb_zca_zcb_zcmp -mabi=ilp32 \
        -c "src/$src.c" -o "$out/hazard3-$src.o"
done

if $NM -u "$out"/*.o | grep -E ' (memcpy|memset)$'; then
    echo 'freestanding target unexpectedly references libc memory helpers' >&2
    exit 1
fi
if $NM -u "$out"/*.o | grep -q 'xxh32_m0plus_'; then
    echo 'M0+ assembly leaked into generic RP2350 objects' >&2
    exit 1
fi

printf 'portable target compile tests OK\n'
