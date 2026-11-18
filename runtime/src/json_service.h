#ifndef RUNTIME_JSON_SERVICE_H
#define RUNTIME_JSON_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "spisor/status.h"

/** Answers the result, or NULL with *refusal set to {message, fix}; NULL with no refusal is out of memory. */
typedef cJSON *(*runtime_json_operation)(const cJSON *argument, cJSON **refusal);

/** Whether refusal is an object carrying a string message and a string fix, the shape every service refuses with. */
int runtime_is_refusal(const cJSON *refusal);

/** The {message, fix} refusal of one message and its fix; NULL when memory runs out. */
cJSON *runtime_refusal(const char *message, const char *fix);

/**
 * The {message, fix} refusal a definition's refusal carries, for a caller that still answers JSON refusals; NULL when
 * the refusal is empty, as an answer of OUT_OF_MEMORY leaves it, or memory runs out.
 */
cJSON *runtime_refusal_of(const spisor_refusal *refusal);

/**
 * Calls an operation of this library with argument, which it deletes: its answer, or NULL with *refusal as the
 * operation set it, NULL when memory ran out. A NULL argument, as a builder answers when memory runs out, is answered
 * as memory running out.
 */
cJSON *runtime_call_operation(runtime_json_operation operation, cJSON *argument, cJSON **refusal);

/**
 * Adds item to object under name, and deletes item when it cannot; 0 when item is NULL or memory runs out.
 *
 * @implNote cJSON_AddItemToObject copies the key and, when that copy fails, leaves the item with its caller.
 */
int runtime_add_owned(cJSON *object, const char *name, cJSON *item);

/**
 * Adds to object under name a reference to item, an object or an array, which object never frees; 0 when item is
 * NULL or memory runs out.
 *
 * @implNote cJSON_AddItemReferenceToObject leaks the reference it made when the key cannot be copied.
 */
int runtime_add_reference(cJSON *object, const char *name, const cJSON *item);

#endif
