#ifndef SPISOR_ALLOCATION_H
#define SPISOR_ALLOCATION_H

#include <stdint.h>

#include "spisor/allocation.h"

#ifdef SPISOR_TESTING
/** Makes the given allocation from now fail, 1 being the next; 0 disarms. Resets the attempt count either way. */
void spisor_memory_fail_at(uint64_t attempt);
/** As spisor_memory_fail_at, and every allocation after that one fails too. */
void spisor_memory_fail_from(uint64_t attempt);
int spisor_memory_failed(void);
uint64_t spisor_memory_attempts(void);
int64_t spisor_memory_live_blocks(void);
#endif

#endif
