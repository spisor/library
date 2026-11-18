#ifndef SPISOR_ALLOCATION_PUBLIC_H
#define SPISOR_ALLOCATION_PUBLIC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The one allocator of a binary: every block the library answers is released with spisor_memory_release. A binary
 * links exactly one definition of it, so a test build can count every attempt and make any one of them fail.
 */
void *spisor_memory_allocate(size_t bytes);
void spisor_memory_release(void *block);
/** realloc through the same allocator; NULL leaves block untouched. */
void *spisor_memory_reallocate(void *block, size_t bytes);
/** calloc through the same allocator, one attempt; NULL when count * size overflows. */
void *spisor_memory_allocate_zeroed(size_t count, size_t size);
/** A copy of text, NULL only when memory runs out. */
char *spisor_memory_duplicate_text(const char *text);

#ifdef __cplusplus
}
#endif

#endif
