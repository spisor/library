#ifndef SPISOR_TEST_ALLOCATION_FAILURE_H
#define SPISOR_TEST_ALLOCATION_FAILURE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "allocation.h"
#include "spisor/status.h"

/**
 * How a failure loop arms the allocator. Every loop runs both: one failed allocation finds a failure answered
 * wrongly, and memory staying out finds a failure path, a cleanup or an undo, that itself needs memory.
 */
typedef enum allocation_failure_mode {
    FAIL_ONE_ALLOCATION,
    FAIL_EVERY_LATER_ALLOCATION,
    ALLOCATION_FAILURE_MODES
} allocation_failure_mode;

/** Fails allocation `attempt` as the mode says; 0 disarms in either mode. */
static inline void fail_allocation(allocation_failure_mode mode, uint64_t attempt) {
    if (mode == FAIL_EVERY_LATER_ALLOCATION && attempt != 0) {
        spisor_memory_fail_from(attempt);
    } else {
        spisor_memory_fail_at(attempt);
    }
}

static inline const char *allocation_failure_mode_name(allocation_failure_mode mode) {
    return mode == FAIL_EVERY_LATER_ALLOCATION ? " sticky" : "";
}

static inline void in_every_failure_mode(void (*failure_loop)(allocation_failure_mode mode)) {
    for (int mode = FAIL_ONE_ALLOCATION; mode < ALLOCATION_FAILURE_MODES; mode++) {
        failure_loop((allocation_failure_mode)mode);
    }
}

/** Zeroes an answer whole, padding included, and sets its `size`, so empty_unless_answered can read every byte. */
static inline void start_empty(void *answer, size_t bytes) {
    uint32_t size = (uint32_t)bytes;
    memset(answer, 0, bytes);
    memcpy(answer, &size, sizeof size);
}

/** What an operation answers in place of an OUT_OF_MEMORY whose answer was not left empty, which no loop accepts. */
#define NOT_EMPTY_OUT_OF_MEMORY ((spisor_status)98)

/**
 * `status`, or NOT_EMPTY_OUT_OF_MEMORY for an OUT_OF_MEMORY whose answer of `bytes`, begun by start_empty, holds
 * anything past its `size`.
 *
 * @implNote Every byte, not only `storage`: a count left beside storage the call freed sends a caller's read past it.
 */
static inline spisor_status empty_unless_answered(spisor_status status, const void *answer, size_t bytes) {
    const unsigned char *byte = answer;
    if (status != SPISOR_OUT_OF_MEMORY) {
        return status;
    }
    for (size_t index = sizeof(uint32_t); index < bytes; index++) {
        if (byte[index] != 0) {
            fprintf(stderr, "an out-of-memory answer kept byte %zu set\n", index);
            return NOT_EMPTY_OUT_OF_MEMORY;
        }
    }
    return status;
}

#endif
