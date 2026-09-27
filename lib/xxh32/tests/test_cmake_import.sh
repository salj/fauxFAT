#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP=${TMPDIR:-/tmp}/xxh32-cmake-$$
trap 'rm -rf "$TMP"' EXIT INT TERM
mkdir -p "$TMP/generic" "$TMP/pico" "$TMP/auto"

cat > "$TMP/generic/CMakeLists.txt" <<EOC
cmake_minimum_required(VERSION 3.13)
project(xxh32_consumer C)
add_subdirectory("$ROOT" xxh32)
add_executable(consumer main.c)
target_link_libraries(consumer PRIVATE xxh32::xxhash)
get_target_property(sel xxh32_selected_backend INTERFACE_COMPILE_DEFINITIONS)
file(WRITE "\${CMAKE_BINARY_DIR}/selection.txt" "\${sel}")
EOC
cat > "$TMP/generic/main.c" <<'EOC'
#include <xxhash.h>
int main(void) { return XXH32("abc", 3, 0) == 0x32d153ffU ? 0 : 1; }
EOC
cmake -S "$TMP/generic" -B "$TMP/generic-build" -DXXH32_BACKEND=GENERIC >/dev/null
cmake --build "$TMP/generic-build" >/dev/null
"$TMP/generic-build/consumer"
grep -q XXH32_BACKEND_GENERIC "$TMP/generic-build/selection.txt"
if grep -q "^CMAKE_ASM_COMPILER" "$TMP/generic-build/CMakeCache.txt"; then
    echo "generic import unnecessarily enabled an assembler" >&2
    exit 1
fi

cat > "$TMP/auto/CMakeLists.txt" <<EOC
cmake_minimum_required(VERSION 3.13)
project(xxh32_auto C)
add_subdirectory("$ROOT" xxh32)
get_target_property(sel xxh32_selected_backend INTERFACE_COMPILE_DEFINITIONS)
file(WRITE "\${CMAKE_BINARY_DIR}/selection.txt" "\${sel}")
EOC
cmake -S "$TMP/auto" -B "$TMP/auto-build" >/dev/null
grep -q XXH32_BACKEND_GENERIC "$TMP/auto-build/selection.txt"

cat > "$TMP/pico/CMakeLists.txt" <<EOC
cmake_minimum_required(VERSION 3.13)
project(xxh32_pico_probe C)
set(PICO_RP2040 1 CACHE INTERNAL "")
add_subdirectory("$ROOT" xxh32)
get_target_property(sel xxh32_selected_backend INTERFACE_COMPILE_DEFINITIONS)
file(WRITE "\${CMAKE_BINARY_DIR}/selection.txt" "\${sel}")
EOC
cmake -S "$TMP/pico" -B "$TMP/pico-build" >/dev/null
grep -q XXH32_BACKEND_M0PLUS "$TMP/pico-build/selection.txt"

printf 'cmake import tests OK\n'
