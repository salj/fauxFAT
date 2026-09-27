#include <stdlib.h>
#include "xxh32_internal.h"

#ifndef XXH_NO_STREAM
XXH32_state_t *XXH32_createState(void)
{
    return (XXH32_state_t *)malloc(sizeof(XXH32_state_t));
}

XXH_errorcode XXH32_freeState(XXH32_state_t *state)
{
    free(state);
    return XXH_OK;
}
#endif
