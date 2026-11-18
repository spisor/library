#ifndef RUNTIME_UPDATES_DEFINITION_H
#define RUNTIME_UPDATES_DEFINITION_H

#include "cJSON.h"
#include "runtime/updates.h"
#include "spisor/status.h"

/**
 * Fills every field of `answer` but its `size` and `storage` from what updates' apply answered, into `storage`.
 *
 * @return OK; OUT_OF_MEMORY, what was taken left in `storage` for its owner to release.
 */
spisor_status runtime_fill_apply_plan(void **storage, const cJSON *answered, runtime_updates_apply_answer *answer);

#endif
