#ifndef SPISOR_STATUS_H
#define SPISOR_STATUS_H

/**
 * @file
 * The status and the refusal every library definition answers with, mirroring the convention of weivel/standard's
 * services.h without depending on it: a library depends on nothing above it.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum spisor_status {
    SPISOR_OK = 0,
    SPISOR_NONE = 1,
    SPISOR_MALFORMED = 2,
    SPISOR_REFUSED = 3,
    SPISOR_OUT_OF_MEMORY = 4
} spisor_status;

/** Why a provider declined, as English UTF-8 the provider owns until its release. First shape ends at `storage`. */
typedef struct spisor_refusal {
    uint32_t size;
    const char *detail;
    const char *fix;
    void *storage;
} spisor_refusal;

#ifdef __cplusplus
}
#endif

#endif
