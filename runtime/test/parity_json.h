#ifndef RUNTIME_TEST_PARITY_JSON_H
#define RUNTIME_TEST_PARITY_JSON_H

#include <stddef.h>

#include "cJSON.h"
#include "spisor/value.h"

/**
 * @file
 * The parity observer's JSON work: an answer in the recorded form, and where two forms first differ. Test code
 * only.
 */

/** A value tree as JSON; NULL for a value of kind NONE and when memory runs out. */
cJSON *parity_json_of_value(const spisor_value *value);

/** The directories a case was laid out in, written back as `{home}` and `{root}`. */
typedef struct parity_tokens {
    const char *home;
    const char *root;
} parity_tokens;

/**
 * Rewrites every text value under `json`, never a key, as the recorded answers hold it: each backslash a slash, then
 * the home written as `{home}` and the root as `{root}`, so an answer reads the same on every OS. 1 when done.
 */
int parity_normalise(cJSON *json, const parity_tokens *tokens);

/** Where two JSON values first differ, and the two values there; `path` is empty when they hold the same value. */
typedef struct parity_difference {
    char path[1024];
    const cJSON *observed;
    const cJSON *expected;
} parity_difference;

/**
 * Whether `observed` and `expected` differ, filling `difference` with the first path at which they do: arrays item by
 * item, objects by key in any order, a member absent on one side found at its key with that side NULL.
 */
int parity_first_difference(const cJSON *observed, const cJSON *expected, parity_difference *difference);

#endif
