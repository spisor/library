#ifndef RUNTIME_STANDARD_REFUSAL_H
#define RUNTIME_STANDARD_REFUSAL_H

/**
 * @file
 * How runtime's standard services answer through `weivel/services.h`'s conventions: each operation builds its refusal
 * in a spisor_refusal of its own and hands it over last, so runtime's refusal helpers serve the standard's structs.
 */

#include <stddef.h>

#include <weivel/services.h>

#include "spisor/status.h"

/** Whether a consumer's refusal may be filled: NULL for none, or reaching its first shape and holding nothing. */
int runtime_is_usable_service_refusal(const weivel_standard_service_refusal *refusal);

/**
 * Answers `status`: moves what `filled` holds into the consumer's `refusal`, zeroing whatever a newer consumer's
 * refusal has beyond the provider's own struct, or frees it when the consumer handed in none, leaving `filled` empty.
 *
 * @implNote spisor_status numbers its answers as weivel_status does, which restatement_test holds.
 */
weivel_status runtime_answer_service(spisor_status status, spisor_refusal *filled,
                                     weivel_standard_service_refusal *refusal);

/** Zeroes the `size` bytes of a consumer's answer beyond the `provided` bytes of the provider's own struct. */
void runtime_zero_beyond(void *answer, size_t provided);

/** Every standard service's release for a refusal it filled; `context` is not read. */
void runtime_release_service_refusal(void *context, weivel_standard_service_refusal *refusal);

#endif
