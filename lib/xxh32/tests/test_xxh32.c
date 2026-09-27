#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"

#include <assert.h>
#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t (*ref_xxh32_fn)(const void *, size_t, uint32_t);

_Static_assert(sizeof(XXH32_state_t) == 48, "upstream-compatible XXH32 state layout expected");
_Static_assert(offsetof(XXH32_state_t, buffer) % 4 == 0, "stream buffer must be word-aligned");

static ref_xxh32_fn load_reference(void)
{
    void *h = dlopen("libxxhash.so.0", RTLD_NOW | RTLD_LOCAL);
    ref_xxh32_fn fn;
    assert(h != NULL);
    *(void **)(&fn) = dlsym(h, "XXH32");
    assert(fn != NULL);
    return fn;
}

static void fill_pattern(uint8_t *buf, size_t len)
{
    uint32_t x = 0x243f6a88u;
    size_t i;
    for (i = 0; i < len; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        buf[i] = (uint8_t)(x >> 11);
    }
}

static void check_oneshot(ref_xxh32_fn ref, const uint8_t *p, size_t len, uint32_t seed)
{
    uint32_t got = XXH32(p, len, seed);
    uint32_t want = ref(p, len, seed);
    if (got != want) {
        fprintf(stderr, "oneshot mismatch len=%zu seed=%08x got=%08x want=%08x\n",
                len, seed, got, want);
        assert(got == want);
    }
}

static void check_stream_chunks(ref_xxh32_fn ref, const uint8_t *p, size_t len,
                                uint32_t seed, size_t chunk)
{
    XXH32_state_t s;
    size_t off = 0;
    uint32_t want = ref(p, len, seed);

    assert(XXH32_reset(&s, seed) == XXH_OK);
    assert(XXH32_update(&s, NULL, 0) == XXH_OK);
    while (off < len) {
        size_t n = len - off;
        if (n > chunk)
            n = chunk;
        assert(XXH32_update(&s, p + off, n) == XXH_OK);
        off += n;
    }

    assert(XXH32_digest(&s) == want);
    assert(XXH32_digest(&s) == want);
}

static void check_stream_split(ref_xxh32_fn ref, const uint8_t *p, size_t len,
                               uint32_t seed, size_t split)
{
    XXH32_state_t s;
    uint32_t want = ref(p, len, seed);

    assert(XXH32_reset(&s, seed) == XXH_OK);
    assert(XXH32_update(&s, p, split) == XXH_OK);
    assert(XXH32_digest(&s) == ref(p, split, seed));
    assert(XXH32_update(&s, p + split, len - split) == XXH_OK);
    assert(XXH32_digest(&s) == want);
}

int main(void)
{
    static const uint32_t seeds[] = {
        0u, 1u, 0x9e3779b1u, 0xffffffffu, 0xdeadbeefu,
    };
    static const size_t chunks[] = {
        1u, 2u, 3u, 4u, 7u, 15u, 16u, 17u, 31u, 32u, 63u,
    };
    uint8_t storage[1024 + 8];
    ref_xxh32_fn ref = load_reference();
    size_t seed_i, off, len, chunk_i, split;

    fill_pattern(storage, sizeof storage);

    assert(XXH32(NULL, 0, 0) == UINT32_C(0x02cc5d05));
    assert(XXH32("a", 1, 0) == UINT32_C(0x550d7456));
    assert(XXH32("abc", 3, 0) == UINT32_C(0x32d153ff));

    for (seed_i = 0; seed_i < sizeof seeds / sizeof seeds[0]; ++seed_i) {
        check_oneshot(ref, NULL, 0, seeds[seed_i]);
        for (off = 0; off < 8; ++off) {
            for (len = 0; len <= 257; ++len)
                check_oneshot(ref, storage + off, len, seeds[seed_i]);
        }
    }

    for (seed_i = 0; seed_i < sizeof seeds / sizeof seeds[0]; ++seed_i) {
        for (off = 0; off < 4; ++off) {
            for (len = 0; len <= 129; ++len) {
                for (chunk_i = 0; chunk_i < sizeof chunks / sizeof chunks[0]; ++chunk_i)
                    check_stream_chunks(ref, storage + off, len, seeds[seed_i], chunks[chunk_i]);
            }
        }
    }

    for (seed_i = 0; seed_i < sizeof seeds / sizeof seeds[0]; ++seed_i) {
        for (len = 0; len <= 64; ++len) {
            for (split = 0; split <= len; ++split)
                check_stream_split(ref, storage + 1, len, seeds[seed_i], split);
        }
    }

    {
        XXH32_state_t s;
        assert(XXH32_reset(&s, 0) == XXH_OK);
        s.total_len_32 = UINT32_MAX - 3u;
        assert(XXH32_update(&s, storage, 4) == XXH_OK);
        assert(s.total_len_32 == 0u);
    }

    puts("xxh32 tests OK");
    return 0;
}
