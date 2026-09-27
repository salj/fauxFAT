#define XXH_STATIC_LINKING_ONLY
#include "../include/xxhash.h"
#ifndef XXH32_EMBEDDED_SUBSET
#error expected local xxh32 subset header
#endif

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(XXH32_hash_t) == 4, "XXH32_hash_t must be 32-bit");
_Static_assert(sizeof(XXH32_canonical_t) == 4, "canonical hash must be four bytes");
_Static_assert(sizeof(XXH32_state_t) == 48, "upstream-compatible XXH32 state layout expected");

static void check_stream(void)
{
    static const unsigned char a[] = "streaming ";
    static const unsigned char b[] = "xxh32";
    unsigned char joined[sizeof a + sizeof b - 2];
    XXH32_state_t s, copy;
    XXH32_hash_t expected;

    memcpy(joined, a, sizeof a - 1);
    memcpy(joined + sizeof a - 1, b, sizeof b - 1);
    expected = XXH32(joined, sizeof joined, 0x12345678u);

    assert(XXH32_reset(&s, 0x12345678u) == XXH_OK);
    assert(XXH32_update(&s, a, sizeof a - 1) == XXH_OK);
    XXH32_copyState(&copy, &s);
    assert(XXH32_update(&s, b, sizeof b - 1) == XXH_OK);
    assert(XXH32_digest(&s) == expected);
    assert(XXH32_digest(&copy) == XXH32(a, sizeof a - 1, 0x12345678u));
}

static void check_alloc(void)
{
    XXH32_state_t *s = XXH32_createState();
    assert(s != NULL);
    assert(XXH32_reset(s, 0) == XXH_OK);
    assert(XXH32_update(s, "abc", 3) == XXH_OK);
    assert(XXH32_digest(s) == UINT32_C(0x32d153ff));
    assert(XXH32_freeState(s) == XXH_OK);
}

static void check_canonical(void)
{
    XXH32_canonical_t c;
    XXH32_canonicalFromHash(&c, UINT32_C(0x12345678));
    assert(c.digest[0] == 0x12 && c.digest[1] == 0x34);
    assert(c.digest[2] == 0x56 && c.digest[3] == 0x78);
    assert(XXH32_hashFromCanonical(&c) == UINT32_C(0x12345678));
}

int main(void)
{
    assert(XXH32(NULL, 0, 0) == UINT32_C(0x02cc5d05));
    assert(XXH32("abc", 3, 0) == UINT32_C(0x32d153ff));
    check_stream();
    check_alloc();
    check_canonical();
    puts("public API tests OK");
    return 0;
}
