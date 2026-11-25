#include "naula/memory.h"

#include "allocation.h"

void *naula_memory_allocate(size_t bytes) {
    return spisor_memory_allocate(bytes);
}

void *naula_memory_reallocate(void *block, size_t bytes) {
    return spisor_memory_reallocate(block, bytes);
}

void naula_memory_release(void *block) {
    spisor_memory_release(block);
}

char *naula_memory_duplicate_text(const char *text) {
    return spisor_memory_duplicate_text(text);
}
