#define XXH_NAMESPACE tiny_
#define XXH_STATIC_LINKING_ONLY
#include "../include/xxhash.h"
#ifndef XXH32_EMBEDDED_SUBSET
#error expected local xxh32 subset header
#endif

#include <stdint.h>

int main(void)
{
    XXH32_state_t state;
    if (XXH32_reset(&state, 0) != XXH_OK) return 1;
    if (XXH32_update(&state, "abc", 3) != XXH_OK) return 2;
    return XXH32_digest(&state) == UINT32_C(0x32d153ff) ? 0 : 3;
}
