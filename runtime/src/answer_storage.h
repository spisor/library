#ifndef RUNTIME_ANSWER_STORAGE_H
#define RUNTIME_ANSWER_STORAGE_H

#include <stddef.h>

#include "cJSON.h"
#include "spisor/status.h"
#include "runtime/registry.h"
#include "spisor/value.h"

/**
 * @file
 * The storage of an answer that holds more than one block: every block taken for it is chained to `*storage`, the
 * answer's own `storage` field, and freed together by runtime_storage_release. A definition fills an answer through
 * these and, when any of them fails, releases what it took, so a failed answer holds nothing.
 */

/** A zeroed block of `bytes` chained to `*storage`; NULL when memory runs out. */
void *runtime_storage_take(void **storage, size_t bytes);

/** A zeroed array of `count` elements of `width` bytes, NULL for none; 0 when memory runs out. */
int runtime_storage_take_array(void **storage, size_t count, size_t width, void **array);

/** Copies `text` into the storage, a NULL text staying NULL; 0 when memory runs out. */
int runtime_storage_copy(void **storage, const char *text, const char **copy);

/**
 * Copies a JSON array of strings into an array of texts, skipping any other item; an absent array answers none.
 *
 * @return 1; 0 when memory runs out.
 */
int runtime_storage_texts(void **storage, const cJSON *array, const char *const **texts, size_t *count);

/**
 * A value tree of `json` in the storage, NULL for a NULL `json`.
 *
 * @return OK; MALFORMED for JSON no value tree carries; OUT_OF_MEMORY.
 */
spisor_status runtime_storage_value(void **storage, const cJSON *json, const spisor_value **value);

/** Frees every block chained to `storage`. NULL does nothing. */
void runtime_storage_release(void *storage);

/**
 * Frees what an answer's storage holds and empties the answer: an answer whose first shape of `shape` bytes begins
 * with its `size` and ends at its `storage`. NULL, or an answer short of that shape, is left as it is.
 */
void runtime_release_stored_answer(void *answer, size_t shape);

/**
 * A host's provenance as a registration records it, {method, version, location, os, architecture, installedAt,
 * package?, options?}, in the storage; NULL for JSON that is not an object.
 *
 * @return OK; MALFORMED for options no value tree carries; OUT_OF_MEMORY.
 */
spisor_status runtime_storage_provenance(void **storage, const cJSON *json, const runtime_registry_provenance **out);

#endif
