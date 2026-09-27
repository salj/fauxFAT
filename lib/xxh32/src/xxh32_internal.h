#ifndef XXH32_INTERNAL_H
#define XXH32_INTERNAL_H

#ifndef XXH_STATIC_LINKING_ONLY
#define XXH_STATIC_LINKING_ONLY 1
#endif
#include "xxhash.h"

#define XXH32_P1 UINT32_C(0x9e3779b1)
#define XXH32_P2 UINT32_C(0x85ebca77)
#define XXH32_P3 UINT32_C(0xc2b2ae3d)
#define XXH32_P4 UINT32_C(0x27d4eb2f)
#define XXH32_P5 UINT32_C(0x165667b1)

#if defined(__GNUC__) || defined(__clang__)
#define XXH32_NOINLINE __attribute__((noinline))
#else
#define XXH32_NOINLINE
#endif

static inline uint32_t xxh32_rotl(uint32_t x, unsigned n)
{
    return (x << n) | (x >> (32u - n));
}

static inline uint32_t xxh32_read32le(const uint8_t *p)
{
#if defined(__GNUC__) || defined(__clang__)
    uint32_t v;
    __builtin_memcpy(&v, p, sizeof v);
# if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return __builtin_bswap32(v);
# else
    return v;
# endif
#else
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
#endif
}

static inline uint32_t xxh32_round(uint32_t acc, uint32_t lane)
{
    acc += lane * XXH32_P2;
    acc = xxh32_rotl(acc, 13);
    acc *= XXH32_P1;
    return acc;
}

static inline void xxh32_init_acc(uint32_t acc[4], uint32_t seed)
{
    acc[0] = seed + XXH32_P1 + XXH32_P2;
    acc[1] = seed + XXH32_P2;
    acc[2] = seed;
    acc[3] = seed - XXH32_P1;
}

static inline void xxh32_rounds_portable(uint32_t acc[4],
                                         const uint8_t *p,
                                         const uint8_t *end)
{
    while (p != end) {
        acc[0] = xxh32_round(acc[0], xxh32_read32le(p + 0));
        acc[1] = xxh32_round(acc[1], xxh32_read32le(p + 4));
        acc[2] = xxh32_round(acc[2], xxh32_read32le(p + 8));
        acc[3] = xxh32_round(acc[3], xxh32_read32le(p + 12));
        p += 16;
    }
}

static inline uint32_t xxh32_converge(const uint32_t acc[4])
{
    return xxh32_rotl(acc[0], 1)
         + xxh32_rotl(acc[1], 7)
         + xxh32_rotl(acc[2], 12)
         + xxh32_rotl(acc[3], 18);
}

static inline uint32_t xxh32_finalize(uint32_t h, const uint8_t *p,
                                      size_t len, uint32_t total_len)
{
    h += total_len;

    while (len >= 4) {
        h += xxh32_read32le(p) * XXH32_P3;
        h = xxh32_rotl(h, 17) * XXH32_P4;
        p += 4;
        len -= 4;
    }

    while (len != 0) {
        h += (uint32_t)*p++ * XXH32_P5;
        h = xxh32_rotl(h, 11) * XXH32_P1;
        --len;
    }

    h ^= h >> 15;
    h *= XXH32_P2;
    h ^= h >> 13;
    h *= XXH32_P3;
    h ^= h >> 16;
    return h;
}

static inline void xxh32_copy_bytes(uint8_t *dst, const uint8_t *src,
                                    size_t len)
{
    while (len-- != 0)
        *dst++ = *src++;
}

#endif
