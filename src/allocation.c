#include "allocation.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef SPISOR_TESTING

static uint64_t countdown;
static uint64_t attempts;
static int failed;
static int sticky;
static int64_t live_blocks;

static int fails_now(void) {
    attempts++;
    if (failed && sticky) {
        return 1;
    }
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

void *spisor_memory_reallocate(void *block, size_t bytes) {
    void *grown;
    if (fails_now()) {
        return NULL;
    }
    grown = realloc(block, bytes);
    if (grown != NULL && block == NULL) {
        live_blocks++;
    }
    return grown;
}

void spisor_memory_fail_at(uint64_t attempt) {
    countdown = attempt;
    attempts = 0;
    failed = 0;
    sticky = 0;
}

void spisor_memory_fail_from(uint64_t attempt) {
    spisor_memory_fail_at(attempt);
    sticky = 1;
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

void *spisor_memory_reallocate(void *block, size_t bytes) {
    return realloc(block, bytes);
}

#endif

void *spisor_memory_allocate_zeroed(size_t count, size_t size) {
    void *block;
    if (size != 0 && count > SIZE_MAX / size) {
        return NULL;
    }
    block = spisor_memory_allocate(count * size);
    if (block != NULL) {
        memset(block, 0, count * size);
    }
    return block;
}

char *spisor_memory_duplicate_text(const char *text) {
    size_t length = strlen(text) + 1;
    char *copy = spisor_memory_allocate(length);
    if (copy != NULL) {
        memcpy(copy, text, length);
    }
    return copy;
}
