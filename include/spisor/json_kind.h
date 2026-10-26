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
 * Reads one whole JSON document exactly as JSON.parse would, or answers NULL. Refuses invalid
 * UTF-8, anything after the value, and a string (key or value, at any depth) holding a NUL.
 *
 * @implNote cJSON departs from JSON.parse three ways, each closed here without touching the
 * vendored file: it accepts invalid UTF-8, so the bytes are validated first; it truncates a string
 * at an escaped NUL, so such a document is refused rather than read short; and it refuses an
 * unpaired surrogate escape, which JSON.stringify emits for an unpaired UTF-16 value, so each one
 * is rewritten to the escape of U+FFFD (what Node's UTF-8 encoding of that string holds) before
 * cJSON reads it.
 * A duplicated key keeps its FIRST position and its LAST value, as JSON.parse does, so every cJSON
 * lookup and iteration over the tree sees what JavaScript sees.
 */
cJSON *spisor_parse_json(const uint8_t *bytes, size_t length);

/**
 * Parses a json kind input with spisor_parse_json. A contextual call must be the envelope
 * {"argument": A, "context": {...}} whose context carries only facts this library knows:
 * environment, home and arguments. Which of them the function declared is the host's
 * business and is not checked here. On any refusal the failure object is written to out.
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

#ifdef __cplusplus
}
#endif

#endif
