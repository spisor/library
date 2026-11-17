#ifndef RUNTIME_DEFINITION_SUPPORT_H
#define RUNTIME_DEFINITION_SUPPORT_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "runtime/environment.h"
#include "spisor/status.h"
#include "spisor/value.h"

/** The bytes of a size-prefixed struct up to and including `field`, the field its first shape ends at. */
#define RUNTIME_SHAPE_THROUGH(type, field) (offsetof(type, field) + sizeof(((type *)0)->field))

/** Whether a struct whose `size` is `size` reaches through its first shape of `shape` bytes. */
int runtime_has_shape(uint32_t size, size_t shape);

/** Whether a consumer's answer can be filled: present, reaching through its first shape, and holding no storage. */
#define RUNTIME_IS_EMPTY_ANSWER(answer, shape) \
    ((answer) != NULL && runtime_has_shape((answer)->size, (shape)) && (answer)->storage == NULL)

/** Zeroes every field of an answer after its `size`, through its first shape of `shape` bytes. */
#define RUNTIME_CLEAR_ANSWER(answer, shape) \
    memset((char *)(answer) + sizeof((answer)->size), 0, (shape) - sizeof((answer)->size))

/** Fills the refusal and answers `status`, or OUT_OF_MEMORY when the refusal could not be filled. */
spisor_status runtime_refuse_as(spisor_refusal *refusal, spisor_status status, const char *detail, const char *fix);

/**
 * What an operation that answers JSON refused with, as a definition answers it: REFUSED with the refusal filled from
 * the JSON's message and fix, or OUT_OF_MEMORY when there is no such refusal, as such an operation answers memory
 * running out, or the refusal could not be filled. Deletes `json_refusal`.
 */
spisor_status runtime_refuse_json(spisor_refusal *refusal, cJSON *json_refusal);

/** The refusal every definition answers for an argument or answer that is absent or short of its first shape. */
spisor_status runtime_refuse_shape(spisor_refusal *refusal, const char *operation);

/** Every definition's release for a refusal it filled. */
void runtime_release_refusal(void *context, spisor_refusal *refusal);

/**
 * Copies `count` texts into one block owned by `*storage`, each NUL-terminated, pointing `packed` at the copies; a NULL
 * text stays NULL. A block holding nothing is still allocated, so a filled answer always has storage.
 *
 * @return 1, or 0 with `*storage` NULL when memory runs out.
 */
int runtime_pack_texts(const char *const *texts, const char **packed, size_t count, void **storage);

/**
 * An environment as the JSON object runtime's path rules read, each variable added in order, so the first of a name
 * answers for it.
 *
 * @return OK with a new object, empty for no variables; MALFORMED for a variable without a name or a value, or a count
 * without variables; OUT_OF_MEMORY. `*json` is NULL whenever the answer is not OK.
 */
spisor_status runtime_environment_json(const runtime_variable *environment, size_t count, cJSON **json);

/**
 * A JSON environment object as the variables a definition takes, borrowing its string members and skipping any other,
 * for a caller that holds its environment as JSON. Freed with free; NULL when memory runs out.
 */
runtime_variable *runtime_variables_of_json(const cJSON *environment, size_t *count);

/** The MALFORMED refusal for a place, named as `place` such as "state", that is empty or not an absolute directory. */
spisor_status runtime_refuse_place(spisor_refusal *refusal, const char *place);

/** The refusal runtime_environment_json's MALFORMED answers with. */
spisor_status runtime_refuse_environment(spisor_refusal *refusal);

/**
 * Frees what runtime_pack_texts filled an answer with and empties it: an answer whose first shape of `shape` bytes
 * begins with its `size` and ends at its `storage`. NULL, or an answer short of that shape, is left as it is.
 */
void runtime_release_packed_answer(void *answer, size_t shape);

/** Overwrites a secret's bytes before they are freed, through a volatile pointer so no write is optimised away. */
void runtime_overwrite(void *bytes, size_t length);

/** Overwrites every text and key of a tree, as its release does before freeing a tree that may hold a secret. */
void runtime_overwrite_texts(const spisor_value *value);

/** Overwrites every string and key of a JSON tree, as its owner does before deleting a tree that may hold a secret. */
void runtime_overwrite_json_texts(const cJSON *json);

/** Overwrites every string and key of a JSON tree that may hold a secret, then deletes it. */
void runtime_delete_secret_json(cJSON *json);

/** Frees a block runtime_pack_texts or another definition helper allocated. */
void runtime_release_storage(void *storage);

#endif
