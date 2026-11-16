#ifndef RUNTIME_DEFINITION_SUPPORT_H
#define RUNTIME_DEFINITION_SUPPORT_H

#include <stddef.h>
#include <stdint.h>

#include "spisor/status.h"

/** The bytes of a size-prefixed struct up to and including `field`, the field its first shape ends at. */
#define RUNTIME_SHAPE_THROUGH(type, field) (offsetof(type, field) + sizeof(((type *)0)->field))

/** Whether a struct whose `size` is `size` reaches through its first shape of `shape` bytes. */
int runtime_has_shape(uint32_t size, size_t shape);

/** Whether a consumer's answer can be filled: present, reaching through its first shape, and holding no storage. */
#define RUNTIME_IS_EMPTY_ANSWER(answer, shape) \
    ((answer) != NULL && runtime_has_shape((answer)->size, (shape)) && (answer)->storage == NULL)

/** Zeroes every field of an answer after its `size`, through its first shape of `shape` bytes. */
#define RUNTIME_CLEAR_ANSWER(answer, shape) \
    memset((char *)(answer) + sizeof((answer)->size), 0, (shape) - sizeof((answer)->size))

/** Fills the refusal and answers `status`, or OUT_OF_MEMORY when the refusal could not be filled. */
spisor_status runtime_refuse_as(spisor_refusal *refusal, spisor_status status, const char *detail, const char *fix);

/** The refusal every definition answers for an argument or answer that is absent or short of its first shape. */
spisor_status runtime_refuse_shape(spisor_refusal *refusal, const char *operation);

/** Every definition's release for a refusal it filled. */
void runtime_release_refusal(void *context, spisor_refusal *refusal);

/**
 * Copies `count` texts into one block owned by `*storage`, each NUL-terminated, pointing `packed` at the copies; a NULL
 * text stays NULL. A block holding nothing is still allocated, so a filled answer always has storage.
 *
 * @return 1, or 0 with `*storage` NULL when memory runs out.
 */
int runtime_pack_texts(const char *const *texts, const char **packed, size_t count, void **storage);

/** Frees a block runtime_pack_texts or another definition helper allocated. */
void runtime_release_storage(void *storage);

#endif
