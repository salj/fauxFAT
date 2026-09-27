#ifndef XXHASH_H_5627135585666179
#define XXHASH_H_5627135585666179 1

/*
 * Small XXH32-only implementation with source compatibility for the upstream
 * xxHash v0.8.4 XXH32 API.  Other hash families are intentionally absent.
 */
#define XXH32_EMBEDDED_SUBSET 1

#define XXH_VERSION_MAJOR 0
#define XXH_VERSION_MINOR 8
#define XXH_VERSION_RELEASE 4
#define XXH_VERSION_NUMBER \
    (XXH_VERSION_MAJOR * 100 * 100 + XXH_VERSION_MINOR * 100 + XXH_VERSION_RELEASE)

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef XXH_PUBLIC_API
#define XXH_PUBLIC_API
#endif
#if defined(__GNUC__) || defined(__clang__)
#define XXH_PUREF __attribute__((__pure__))
#define XXH_CONSTF __attribute__((__const__))
#define XXH_MALLOCF __attribute__((__malloc__))
#else
#define XXH_PUREF
#define XXH_CONSTF
#define XXH_MALLOCF
#endif

#ifdef XXH_NAMESPACE
#define XXH_CAT(A, B) A##B
#define XXH_NAME2(A, B) XXH_CAT(A, B)
#define XXH_versionNumber XXH_NAME2(XXH_NAMESPACE, XXH_versionNumber)
#define XXH32 XXH_NAME2(XXH_NAMESPACE, XXH32)
#define XXH32_createState XXH_NAME2(XXH_NAMESPACE, XXH32_createState)
#define XXH32_freeState XXH_NAME2(XXH_NAMESPACE, XXH32_freeState)
#define XXH32_copyState XXH_NAME2(XXH_NAMESPACE, XXH32_copyState)
#define XXH32_reset XXH_NAME2(XXH_NAMESPACE, XXH32_reset)
#define XXH32_update XXH_NAME2(XXH_NAMESPACE, XXH32_update)
#define XXH32_digest XXH_NAME2(XXH_NAMESPACE, XXH32_digest)
#define XXH32_canonicalFromHash XXH_NAME2(XXH_NAMESPACE, XXH32_canonicalFromHash)
#define XXH32_hashFromCanonical XXH_NAME2(XXH_NAMESPACE, XXH32_hashFromCanonical)
#endif

typedef enum {
    XXH_OK = 0,
    XXH_ERROR = 1
} XXH_errorcode;

typedef uint32_t XXH32_hash_t;

XXH_PUBLIC_API XXH_CONSTF unsigned XXH_versionNumber(void);
XXH_PUBLIC_API XXH_PUREF XXH32_hash_t
XXH32(const void *input, size_t length, XXH32_hash_t seed);

#ifndef XXH_NO_STREAM
typedef struct XXH32_state_s XXH32_state_t;

XXH_PUBLIC_API XXH_MALLOCF XXH32_state_t *XXH32_createState(void);
XXH_PUBLIC_API XXH_errorcode XXH32_freeState(XXH32_state_t *statePtr);
XXH_PUBLIC_API void XXH32_copyState(XXH32_state_t *dst_state,
                                    const XXH32_state_t *src_state);
XXH_PUBLIC_API XXH_errorcode XXH32_reset(XXH32_state_t *statePtr,
                                         XXH32_hash_t seed);
XXH_PUBLIC_API XXH_errorcode XXH32_update(XXH32_state_t *statePtr,
                                          const void *input, size_t length);
XXH_PUBLIC_API XXH_PUREF XXH32_hash_t
XXH32_digest(const XXH32_state_t *statePtr);
#endif

typedef struct {
    unsigned char digest[4];
} XXH32_canonical_t;

XXH_PUBLIC_API void XXH32_canonicalFromHash(XXH32_canonical_t *dst,
                                             XXH32_hash_t hash);
XXH_PUBLIC_API XXH_PUREF XXH32_hash_t
XXH32_hashFromCanonical(const XXH32_canonical_t *src);

#ifndef XXH_NO_STREAM
#if defined(XXH_STATIC_LINKING_ONLY) || defined(XXH_INLINE_ALL) || \
    defined(XXH_PRIVATE_API) || defined(XXH_IMPLEMENTATION)
struct XXH32_state_s {
    XXH32_hash_t total_len_32;
    XXH32_hash_t large_len;
    XXH32_hash_t acc[4];
    unsigned char buffer[16];
    XXH32_hash_t bufferedSize;
    XXH32_hash_t reserved;
};
#endif
#endif

/*
 * XXH_INLINE_ALL / XXH_PRIVATE_API / XXH_IMPLEMENTATION are accepted as
 * source-build compatibility knobs, but this library intentionally remains an
 * external compiled implementation.  They therefore add no code to a caller.
 */

#ifdef __cplusplus
}
#endif

/* Fail early when code expects hash families this deliberately tiny library
 * does not provide. This has no runtime or linked-size cost. */
#if (defined(__GNUC__) || defined(__clang__)) && !defined(XXH32_NO_UNSUPPORTED_POISON)
#pragma GCC poison XXH64 XXH64_createState XXH64_freeState XXH64_copyState
#pragma GCC poison XXH64_reset XXH64_update XXH64_digest
#pragma GCC poison XXH64_canonicalFromHash XXH64_hashFromCanonical
#pragma GCC poison XXH3_64bits XXH3_64bits_withSeed XXH3_64bits_withSecret
#pragma GCC poison XXH3_createState XXH3_freeState XXH3_copyState
#pragma GCC poison XXH3_64bits_reset XXH3_64bits_reset_withSeed
#pragma GCC poison XXH3_64bits_reset_withSecret XXH3_64bits_update XXH3_64bits_digest
#pragma GCC poison XXH3_128bits XXH3_128bits_withSeed XXH3_128bits_withSecret
#pragma GCC poison XXH3_128bits_reset XXH3_128bits_reset_withSeed
#pragma GCC poison XXH3_128bits_reset_withSecret XXH3_128bits_update XXH3_128bits_digest
#pragma GCC poison XXH128 XXH128_isEqual XXH128_cmp
#pragma GCC poison XXH128_canonicalFromHash XXH128_hashFromCanonical
#endif

#endif
