/* Build systems sometimes define these while vendoring upstream xxHash.
 * Our external-library implementation deliberately tolerates them. */
#define XXH_INLINE_ALL
#define XXH_PRIVATE_API
#define XXH_IMPLEMENTATION
#include "../include/xxhash.h"
#ifndef XXH32_EMBEDDED_SUBSET
#error expected local xxh32 subset header
#endif

int main(void)
{
    XXH32_state_t state;
    return sizeof(state) == 48 ? 0 : 1;
}
