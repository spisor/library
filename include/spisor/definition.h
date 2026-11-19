#ifndef SPISOR_DEFINITION_H
#define SPISOR_DEFINITION_H

/**
 * @file
 * What every library's definitions share: reading a size-prefixed struct only through its first shape, taking only an
 * empty answer, and refusing the same way.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "spisor/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The bytes of a size-prefixed struct up to and including `field`, the field its first shape ends at. */
#define SPISOR_SHAPE_THROUGH(type, field) (offsetof(type, field) + sizeof(((type *)0)->field))

/** Whether a struct whose `size` is `size` reaches through its first shape of `shape` bytes. */
int spisor_has_shape(uint32_t size, size_t shape);

/** Whether a consumer's answer can be filled: present, reaching through its first shape, and holding no storage. */
#define SPISOR_IS_EMPTY_ANSWER(answer, shape) \
    ((answer) != NULL && spisor_has_shape((answer)->size, (shape)) && (answer)->storage == NULL)

/**
 * SPISOR_IS_EMPTY_ANSWER for an answer holding arrays, whose counts, joined with `|`, must all be zero too.
 *
 * @implNote A fill appends at its array's count, so a nonzero count beside NULL storage would write past the array.
 */
#define SPISOR_IS_EMPTY_COUNTED_ANSWER(answer, shape, counts) \
    (SPISOR_IS_EMPTY_ANSWER(answer, shape) && (counts) == 0)

/** Zeroes every field of an answer after its `size`, through its first shape of `shape` bytes. */
#define SPISOR_CLEAR_ANSWER(answer, shape) \
    memset((char *)(answer) + sizeof((answer)->size), 0, (shape) - sizeof((answer)->size))

/** Fills the refusal and answers `status`, or OUT_OF_MEMORY when the refusal could not be filled. */
spisor_status spisor_refuse_as(spisor_refusal *refusal, spisor_status status, const char *detail, const char *fix);

/**
 * The refusal every definition answers for an argument or answer that is absent or short of its first shape, or an
 * answer that is not empty, naming `operation` such as "paths' resolve".
 */
spisor_status spisor_refuse_shape(spisor_refusal *refusal, const char *operation);

/** A definition's release for a refusal it filled through spisor_refuse; `context` is not read. */
void spisor_release_refusal(void *context, spisor_refusal *refusal);

/** Overwrites a secret's bytes before they are freed, through a volatile pointer so no write is optimised away. */
void spisor_overwrite(void *bytes, size_t length);

#ifdef __cplusplus
}
#endif

#endif
