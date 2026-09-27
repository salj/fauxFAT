#include "xxh32_internal.h"

#ifndef XXH_NO_STREAM
void XXH32_copyState(XXH32_state_t *dst, const XXH32_state_t *src)
{
    dst->total_len_32 = src->total_len_32;
    dst->large_len = src->large_len;
    dst->acc[0] = src->acc[0];
    dst->acc[1] = src->acc[1];
    dst->acc[2] = src->acc[2];
    dst->acc[3] = src->acc[3];
    dst->buffer[0] = src->buffer[0];
    dst->buffer[1] = src->buffer[1];
    dst->buffer[2] = src->buffer[2];
    dst->buffer[3] = src->buffer[3];
    dst->buffer[4] = src->buffer[4];
    dst->buffer[5] = src->buffer[5];
    dst->buffer[6] = src->buffer[6];
    dst->buffer[7] = src->buffer[7];
    dst->buffer[8] = src->buffer[8];
    dst->buffer[9] = src->buffer[9];
    dst->buffer[10] = src->buffer[10];
    dst->buffer[11] = src->buffer[11];
    dst->buffer[12] = src->buffer[12];
    dst->buffer[13] = src->buffer[13];
    dst->buffer[14] = src->buffer[14];
    dst->buffer[15] = src->buffer[15];
    dst->bufferedSize = src->bufferedSize;
    dst->reserved = src->reserved;
}
#endif
