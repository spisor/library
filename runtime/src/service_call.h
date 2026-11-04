#ifndef RUNTIME_SERVICE_CALL_H
#define RUNTIME_SERVICE_CALL_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

typedef int32_t (*runtime_json_function)(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/**
 * Calls one json function of this library in-process with argument, answering its parsed result, a
 * refusal's {message, fix} included, with the status in *status. NULL when memory runs out.
 */
cJSON *runtime_call_service(runtime_json_function function, const cJSON *argument, int32_t *status);

#endif
