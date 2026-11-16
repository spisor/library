#ifndef SPISOR_STATUS_H
#define SPISOR_STATUS_H

/**
 * @file
 * The status and the refusal every library definition answers with. The numbers and the refusal's shape are
 * weivel/standard's services.h convention, restated here because a library depends on nothing above it.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** An empty size-prefixed struct: its `size` set, every other field zero. */
#define SPISOR_INIT(type) { .size = (uint32_t)sizeof(type) }

typedef enum spisor_status {
    SPISOR_OK = 0,
    SPISOR_NONE = 1,
    SPISOR_MALFORMED = 2,
    SPISOR_REFUSED = 3,
    SPISOR_OUT_OF_MEMORY = 4
} spisor_status;

/**
 * Why a provider declined, as English UTF-8 the provider owns until the definition's release for it. An empty refusal
 * has its `size` set and NULL texts and storage, as SPISOR_INIT leaves it. First shape ends at `storage`.
 */
typedef struct spisor_refusal {
    uint32_t size;
    const char *detail;
    const char *fix;
    void *storage;
} spisor_refusal;

/**
 * Fills an empty refusal with copies of `detail` and `fix`, both held in one block its `storage` owns.
 *
 * @param refusal the consumer's empty refusal, or NULL for none. One whose `size` is below its first shape, or whose
 * `storage` is set, is left as it was.
 * @param detail NULL leaves `detail` NULL.
 * @param fix NULL leaves `fix` NULL.
 * @return REFUSED, so a definition answers with it directly; OUT_OF_MEMORY with the refusal left as it was.
 */
spisor_status spisor_refuse(spisor_refusal *refusal, const char *detail, const char *fix);

/** Frees what spisor_refuse filled and leaves the refusal empty. NULL, or an empty refusal, does nothing. */
void spisor_refusal_release(spisor_refusal *refusal);

#ifdef __cplusplus
}
#endif

#endif
