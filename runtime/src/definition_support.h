#ifndef RUNTIME_DEFINITION_SUPPORT_H
#define RUNTIME_DEFINITION_SUPPORT_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "json_service.h"
#include "runtime/environment.h"
#include "runtime/item_version.h"
#include "spisor/definition.h"
#include "spisor/status.h"
#include "spisor/value.h"

/** Fills an answer from what an operation answered; a fill that fails may leave blocks in the answer's storage. */
typedef spisor_status (*runtime_answer_filler)(const cJSON *answered, void *answer);

/**
 * Fills, with `fill`, an answer whose first shape of `answer_shape` bytes ends at its `storage` from `answered`, which
 * it deletes, or refuses as runtime_refuse_json does with `json_refusal` when `answered` is NULL. A failed fill
 * releases what it took, so a failed answer holds nothing.
 */
spisor_status runtime_serve_answered(cJSON *answered, cJSON *json_refusal, runtime_answer_filler fill, void *answer,
                                     size_t answer_shape, spisor_refusal *refusal);

/** Adds `value`, a well-formed tree of `kind`, to `call` as JSON under `name`; MALFORMED for NULL or any other value. */
spisor_status runtime_add_value_json(cJSON *call, const char *name, const spisor_value *value, spisor_value_kind kind);

/** Whether a text is present and not empty. */
int runtime_is_text(const char *text);

/** Whether a place is present and an absolute path. */
int runtime_is_place(const char *place);

/** Whether `count` texts are each present, an empty one allowed, as an argument list holds them. */
int runtime_are_arguments(const char *const *texts, size_t count);

/** Whether `count` texts are each present and not empty, and, when `unique`, each given once. */
int runtime_are_texts(const char *const *texts, size_t count, int unique);

/** Whether `count` item versions each carry an id and a version that are not empty, and no id twice. */
int runtime_are_item_versions(const runtime_item_version *versions, size_t count);

/**
 * Whether `count` variables are an environment a definition takes: each with a name and a value, the name not empty,
 * holding no = and given once. It allocates nothing, so a definition checks it before anything else.
 */
int runtime_is_environment(const runtime_variable *environment, size_t count);

/** Takes an answer's first block of storage, before anything fills it; OUT_OF_MEMORY when memory runs out. */
spisor_status runtime_start_storage(void **storage);

/** The MALFORMED refusal for an argument carrying no usable `member`, whose shape `header` describes. */
spisor_status runtime_refuse_member(spisor_refusal *refusal, const char *member, const char *header);

/**
 * OK for a host id that is a plain file name; otherwise MALFORMED, refused through runtime_refuse_member with
 * `header` for one that is NULL or empty.
 *
 * @implNote Defined in definition_host_target.c, linked beside the host id and platform rules it reads.
 */
spisor_status runtime_check_host_id(spisor_refusal *refusal, const char *host_id, const char *header);

/** OK for a NULL platform or a platform key; otherwise MALFORMED, refused through runtime_refuse_member. */
spisor_status runtime_check_platform(spisor_refusal *refusal, const char *platform, const char *header);

/**
 * What an operation that answers JSON refused with, as a definition answers it: REFUSED with the refusal filled from
 * the JSON's message and fix, or OUT_OF_MEMORY when there is no such refusal, as such an operation answers memory
 * running out, or the refusal could not be filled. Deletes `json_refusal`.
 */
spisor_status runtime_refuse_json(spisor_refusal *refusal, cJSON *json_refusal);

/**
 * Copies `count` texts into one block owned by `*storage`, each NUL-terminated, pointing `packed` at the copies; a NULL
 * text stays NULL. A block holding nothing is still allocated, so a filled answer always has storage.
 *
 * @return 1, or 0 with `*storage` NULL when memory runs out.
 */
int runtime_pack_texts(const char *const *texts, const char **packed, size_t count, void **storage);

/**
 * An environment as the JSON object runtime's path rules read.
 *
 * @return OK with a new object, empty for no variables; MALFORMED for variables runtime_is_environment refuses;
 * OUT_OF_MEMORY. `*json` is NULL whenever the answer is not OK.
 */
spisor_status runtime_environment_json(const runtime_variable *environment, size_t count, cJSON **json);

/**
 * A JSON environment object as the variables a definition takes, borrowing its string members and skipping any other,
 * for a caller that holds its environment as JSON. Freed; NULL when memory runs out.
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

/** Overwrites every text and key of a tree, as its release does before freeing a tree that may hold a secret. */
void runtime_overwrite_texts(const spisor_value *value);

/** Overwrites every string and key of a JSON tree, as its owner does before deleting a tree that may hold a secret. */
void runtime_overwrite_json_texts(const cJSON *json);

/** Frees a block runtime_pack_texts or another definition helper allocated. */
void runtime_release_storage(void *storage);

#endif
