#ifndef SPISOR_JSON_KIND_H
#define SPISOR_JSON_KIND_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPISOR_STATUS_SUCCESS 0
#define SPISOR_STATUS_FAILURE 1
#define SPISOR_STATUS_OUT_OF_MEMORY 2

/**
 * One call of a json kind function, split from its input.
 *
 * @implNote For a contextual call both trees are detached from the envelope, so each is a
 * standalone tree; for a plain call the whole input is the argument and context is NULL.
 * Release both with spisor_discard_call.
 */
typedef struct spisor_call {
    cJSON *argument;
    cJSON *context;
} spisor_call;

/**
 * Parses a json kind input. A contextual call must be the envelope
 * {"argument": A, "context": {"environment": {..}, "home": "..", "arguments": [..]}} carrying
 * only the facts the function declared.
 *
 * @implNote cJSON accepts invalid UTF-8 and finds the FIRST of a duplicated key, so the input is
 * validated as UTF-8 first and every key here resolves to its LAST occurrence, as JSON.parse does.
 * On any refusal the failure object is written to out and the status returned is non-zero.
 */
int32_t spisor_parse_call(const uint8_t *in, size_t in_len, int contextual, spisor_call *call,
                          uint8_t **out, size_t *out_len);

void spisor_discard_call(spisor_call *call);

int32_t spisor_succeed(const cJSON *result, uint8_t **out, size_t *out_len);

int32_t spisor_fail(const char *message, const char *fix, uint8_t **out, size_t *out_len);

/** Frees a buffer this library wrote to out; a library's own release function calls it. */
void spisor_release(uint8_t *buffer);

/** The string fact named, such as "home", or NULL when the context does not carry it. */
const char *spisor_context_string(const spisor_call *call, const char *fact);

/**
 * The environment entry named, or NULL when absent.
 *
 * @implNote Matched case-sensitively, as a lookup in a plain JavaScript object is, even on
 * Windows, because the host hands the environment over as a copied object.
 */
const char *spisor_environment(const spisor_call *call, const char *name);

/** The member of object under key, taking the last of duplicated keys as JSON.parse does. */
cJSON *spisor_last_member(const cJSON *object, const char *key);

#ifdef __cplusplus
}
#endif

#endif
