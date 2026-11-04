#ifndef RUNTIME_JSON_SERVICE_H
#define RUNTIME_JSON_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

/** Answers the result, or NULL with *refusal set to {message, fix}; NULL with no refusal is out of memory. */
typedef cJSON *(*runtime_json_operation)(const cJSON *argument, cJSON **refusal);

/** Parses one plain json call and serves it through operation, answering its result or its refusal. */
int32_t runtime_serve_json(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len,
                           runtime_json_operation operation);

/** Whether refusal is an object carrying a string message and a string fix, the shape every service refuses with. */
int runtime_is_refusal(const cJSON *refusal);

/** The {message, fix} refusal of one message and its fix; NULL when memory runs out. */
cJSON *runtime_refusal(const char *message, const char *fix);

/**
 * Calls another json function of this library with argument, which it frees: its answer, or NULL with
 * *refusal set to the refusal it answered (NULL when memory ran out).
 */
cJSON *runtime_call_or_refuse(int32_t (*function)(const uint8_t *, size_t, uint8_t **, size_t *), cJSON *argument,
                              cJSON **refusal);

#endif
