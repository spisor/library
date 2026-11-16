#ifndef SPISOR_ALLOCATION_H
#define SPISOR_ALLOCATION_H

#include <stddef.h>
#include <stdint.h>

/**
 * The allocator the library's definition types allocate through, so a build for the tests (SPISOR_TESTING) can count
 * the blocks still live, count every attempt, and make any one allocation fail.
 */
void *spisor_memory_allocate(size_t bytes);
void spisor_memory_release(void *block);

#ifdef SPISOR_TESTING
/** Makes the given allocation from now fail, 1 being the next; 0 disarms. Resets the attempt count either way. */
void spisor_memory_fail_at(uint64_t attempt);
int spisor_memory_failed(void);
uint64_t spisor_memory_attempts(void);
int64_t spisor_memory_live_blocks(void);
#endif

#endif
