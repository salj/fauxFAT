#include "xxh32_internal.h"

void XXH32_canonicalFromHash(XXH32_canonical_t *dst, XXH32_hash_t hash)
{
    dst->digest[0] = (unsigned char)(hash >> 24);
    dst->digest[1] = (unsigned char)(hash >> 16);
    dst->digest[2] = (unsigned char)(hash >> 8);
    dst->digest[3] = (unsigned char)hash;
}

XXH32_hash_t XXH32_hashFromCanonical(const XXH32_canonical_t *src)
{
    return ((uint32_t)src->digest[0] << 24)
         | ((uint32_t)src->digest[1] << 16)
         | ((uint32_t)src->digest[2] << 8)
         | (uint32_t)src->digest[3];
}
