#ifndef SPISOR_TEST_ALLOCATION_FAILURE_H
#define SPISOR_TEST_ALLOCATION_FAILURE_H

#include <stdint.h>

#include "allocation.h"

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

#endif
