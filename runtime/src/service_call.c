#include "service_call.h"

#include <string.h>

#include "spisor/json_kind.h"

cJSON *runtime_call_service(runtime_json_function function, const cJSON *argument, int32_t *status) {
    *status = SPISOR_STATUS_OUT_OF_MEMORY;
    char *text = argument == NULL ? NULL : cJSON_PrintUnformatted(argument);
    if (text == NULL) {
        return NULL;
    }
    uint8_t *out = NULL;
    size_t out_len = 0;
    *status = function((const uint8_t *)text, strlen(text), &out, &out_len);
    cJSON_free(text);
    cJSON *answer = out == NULL ? NULL : spisor_parse_json(out, out_len);
    spisor_release(out);
    return answer;
}
