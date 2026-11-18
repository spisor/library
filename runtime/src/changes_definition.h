#ifndef RUNTIME_CHANGES_DEFINITION_H
#define RUNTIME_CHANGES_DEFINITION_H

#include "cJSON.h"
#include "runtime/changes.h"
#include "spisor/status.h"

/**
 * Copies the drifted changes a reversal answered, a JSON array of {sequence, kind, detail}, into `storage`.
 *
 * @return OK; OUT_OF_MEMORY, what was taken left in `storage` for its owner to release.
 */
spisor_status runtime_fill_drifted(void **storage, const cJSON *drifted, const runtime_change_drift **out,
                                  size_t *count);

#endif
