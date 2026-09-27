# xxh32

A small compiled XXH32 library. It implements the upstream xxHash v0.8.4 XXH32 API: one-shot and streaming hashing, state allocation and copying, canonical conversion, and `XXH_versionNumber()`. The implementation is C99; an optional Cortex-M0+ assembly backend accelerates aligned bulk input on RP2040. Unaligned input and tails use the portable C path.

The public header is `include/xxhash.h`. Include it as `<xxhash.h>` and link the library. This is an XXH32-only subset, not a replacement for the full upstream xxHash distribution.

## CMake

Add the source tree and link its target:

```cmake
add_subdirectory(external/xxh32)
target_link_libraries(firmware PRIVATE xxh32::xxhash)
```

`XXH32_BACKEND` defaults to `AUTO`. It selects M0PLUS for the Pico SDK RP2040 target or an ARMv6-M compiler, and GENERIC elsewhere. Set it to `GENERIC` or `M0PLUS` to override selection. `xxh32::generic` is always available; `xxh32::m0plus` is available when selected or when `XXH32_BUILD_M0PLUS_TARGET=ON`.

`XXH32_NAMESPACE` optionally prefixes exported symbols, for example `-DXXH32_NAMESPACE=myfw_`.

## Make and plain C builds

`make` builds the portable static library at `build/generic/libxxh32_generic.a`. Override `CC`, `AR`, `CFLAGS`, or `BUILD` as needed. From another makefile, either link that archive or compile these six sources into your target:

```make
XXH32_DIR := external/xxh32
XXH32_SOURCES := $(addprefix $(XXH32_DIR)/src/, \
	 xxh32_oneshot.c xxh32_stream.c xxh32_copy.c \
	 xxh32_alloc.c xxh32_canonical.c xxh32_version.c)
CPPFLAGS += -I$(XXH32_DIR)/include

app: main.c $(XXH32_SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@
```

For a Pico SDK CMake build, use `xxh32::xxhash`; the RP2040 backend is selected automatically. The standalone `make m0plus` target is for cross-builds using its default Clang ARM flags. In another Makefile, include `src/m0plus/xxh32_oneshot.S` and `src/m0plus/xxh32_stream.S`, define `XXH32_BACKEND_M0PLUS=1` for the C sources, and pass the compiler's Cortex-M0+ target flags.

## Compatibility boundary

Calls and state layout within the supported XXH32 API are compatible with upstream v0.8.4. The library does not provide XXH64, XXH3, XXH128, upstream header-only implementation, SIMD dispatch, or the upstream CLI. Code that needs those should use upstream xxHash.
