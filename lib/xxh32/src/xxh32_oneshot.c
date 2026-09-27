#include "xxh32_internal.h"

#if defined(XXH32_BACKEND_M0PLUS)
extern uint32_t xxh32_m0plus_oneshot_aligned(const uint8_t *p,
                                              const uint8_t *end,
                                              uint32_t seed);
#endif

static XXH32_NOINLINE uint32_t
xxh32_bulk_portable(const uint8_t *p, const uint8_t *end, uint32_t seed)
{
    uint32_t acc[4];
    xxh32_init_acc(acc, seed);
    xxh32_rounds_portable(acc, p, end);
    return xxh32_converge(acc);
}

XXH32_hash_t XXH32(const void *input, size_t len, XXH32_hash_t seed)
{
    const uint8_t *p = (const uint8_t *)input;
    const size_t total = len;
    uint32_t h;

    if (len >= 16) {
        size_t bulk = len & ~(size_t)15;
        const uint8_t *end = p + bulk;

#if defined(XXH32_BACKEND_M0PLUS)
        if ((((uintptr_t)p) & 3u) == 0)
            h = xxh32_m0plus_oneshot_aligned(p, end, seed);
        else
#endif
            h = xxh32_bulk_portable(p, end, seed);

        p = end;
        len -= bulk;
    } else {
        h = seed + XXH32_P5;
    }

    return xxh32_finalize(h, p, len, (uint32_t)total);
}
