#include "xxh32_internal.h"

#ifndef XXH_NO_STREAM
#if defined(XXH32_BACKEND_M0PLUS)
extern void xxh32_m0plus_rounds_aligned(uint32_t acc[4],
                                         const uint8_t *p,
                                         const uint8_t *end);
#endif

static XXH32_NOINLINE void
xxh32_state_rounds_portable(XXH32_state_t *state,
                            const uint8_t *p, const uint8_t *end)
{
    xxh32_rounds_portable(state->acc, p, end);
}

static void xxh32_state_rounds(XXH32_state_t *state,
                               const uint8_t *p, const uint8_t *end)
{
#if defined(XXH32_BACKEND_M0PLUS)
    if ((((uintptr_t)p) & 3u) == 0)
        xxh32_m0plus_rounds_aligned(state->acc, p, end);
    else
#endif
        xxh32_state_rounds_portable(state, p, end);
}

XXH_errorcode XXH32_reset(XXH32_state_t *state, XXH32_hash_t seed)
{
    xxh32_init_acc(state->acc, seed);
    state->total_len_32 = 0;
    state->large_len = 0;
    state->bufferedSize = 0;
    state->reserved = 0;
    return XXH_OK;
}

XXH_errorcode XXH32_update(XXH32_state_t *state, const void *input, size_t len)
{
    const uint8_t *p = (const uint8_t *)input;

    if (len == 0)
        return XXH_OK;

    state->total_len_32 += (uint32_t)len;

    if (len < 16u - state->bufferedSize) {
        xxh32_copy_bytes(state->buffer + state->bufferedSize, p, len);
        state->bufferedSize += (uint32_t)len;
        return XXH_OK;
    }

    if (state->bufferedSize != 0) {
        size_t fill = 16u - state->bufferedSize;
        xxh32_copy_bytes(state->buffer + state->bufferedSize, p, fill);
        xxh32_state_rounds(state, state->buffer, state->buffer + 16);
        state->large_len = 1;
        p += fill;
        len -= fill;
        state->bufferedSize = 0;
    }

    if (len >= 16) {
        size_t bulk = len & ~(size_t)15;
        const uint8_t *end = p + bulk;
        xxh32_state_rounds(state, p, end);
        state->large_len = 1;
        p = end;
        len -= bulk;
    }

    if (len != 0) {
        xxh32_copy_bytes(state->buffer, p, len);
        state->bufferedSize = (uint32_t)len;
    }

    return XXH_OK;
}

XXH32_hash_t XXH32_digest(const XXH32_state_t *state)
{
    uint32_t h;

    if (state->large_len)
        h = xxh32_converge(state->acc);
    else
        h = state->acc[2] + XXH32_P5;

    return xxh32_finalize(h, state->buffer, state->bufferedSize,
                          state->total_len_32);
}

#endif
