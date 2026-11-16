#include "allocation.h"

#include <stdlib.h>

#ifdef SPISOR_TESTING

static uint64_t countdown;
static uint64_t attempts;
static int failed;
static int64_t live_blocks;

static int fails_now(void) {
    attempts++;
    if (countdown == 0) {
        return 0;
    }
    countdown--;
    if (countdown > 0) {
        return 0;
    }
    failed = 1;
    return 1;
}

void *spisor_memory_allocate(size_t bytes) {
    void *block;
    if (fails_now()) {
        return NULL;
    }
    block = malloc(bytes);
    if (block != NULL) {
        live_blocks++;
    }
    return block;
}

void spisor_memory_release(void *block) {
    if (block != NULL) {
        live_blocks--;
    }
    free(block);
}

void spisor_memory_fail_at(uint64_t attempt) {
    countdown = attempt;
    attempts = 0;
    failed = 0;
}

int spisor_memory_failed(void) {
    return failed;
}

uint64_t spisor_memory_attempts(void) {
    return attempts;
}

int64_t spisor_memory_live_blocks(void) {
    return live_blocks;
}

#else

void *spisor_memory_allocate(size_t bytes) {
    return malloc(bytes);
}

void spisor_memory_release(void *block) {
    free(block);
}

#endif
