#ifndef SPISOR_VECTOR_READER_H
#define SPISOR_VECTOR_READER_H

/**
 * @file
 * Turns a JSON test vector into a definition's structs. Test code only: a link-graph check refuses any shipped target
 * that links it, so vectors stay JSON without JSON crossing a definition.
 *
 * Every reader answers 1 when it filled its answer and 0 otherwise, having printed why to stderr naming the member, so
 * a test reports a broken vector rather than a wrong answer.
 */

#include <stddef.h>

#include "cJSON.h"
#include "spisor/status.h"
#include "spisor/value.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A vector file read whole and parsed as JSON.parse would, released with cJSON_Delete; NULL when it cannot be. */
cJSON *spisor_vector_read(const char *path);

/** A member that must be present, of any JSON kind. */
const cJSON *spisor_vector_member(const cJSON *object, const char *key);

/** A text member, borrowed from `object`: its bytes, and their length without the NUL. */
int spisor_vector_text(const cJSON *object, const char *key, const char **text, size_t *length);

/** A status spelled by its name without the prefix: "OK", "NONE", "MALFORMED", "REFUSED" or "OUT_OF_MEMORY". */
int spisor_vector_status(const cJSON *object, const char *key, spisor_status *status);

/**
 * A refusal written as {"detail": text, "fix": text}, either member optional, borrowed from `object` into an empty
 * refusal whose `storage` stays NULL, so it is compared against and never released.
 */
int spisor_vector_refusal(const cJSON *object, const char *key, spisor_refusal *refusal);

/** Any member as a value tree, into an empty one the caller releases with spisor_value_release. */
int spisor_vector_value(const cJSON *object, const char *key, spisor_value_tree *tree);

#ifdef __cplusplus
}
#endif

#endif
