#include "catalog_answer.h"

#include <stdio.h>

#include "spisor/json_kind.h"

int32_t runtime_catalog_answer(cJSON *result, uint8_t **out, size_t *out_len) {
    if (result == NULL) {
        *out = NULL;
        *out_len = 0;
        return SPISOR_STATUS_OUT_OF_MEMORY;
    }
    int32_t status = spisor_succeed(result, out, out_len);
    cJSON_Delete(result);
    return status;
}

int32_t runtime_catalog_refuse_argument(const char *member, const char *shape, uint8_t **out, size_t *out_len) {
    char message[256];
    char fix[512];
    snprintf(message, sizeof message, "the argument carries no usable %s", member);
    snprintf(fix, sizeof fix, "send %s as %s", member, shape);
    return spisor_fail(message, fix, out, out_len);
}

const char *runtime_catalog_text_member(const cJSON *argument, const char *name) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(argument, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

int runtime_catalog_attach(cJSON *object, const char *name, cJSON *item) {
    if (item == NULL) {
        return 0;
    }
    if (object == NULL || !cJSON_AddItemToObject(object, name, item)) {
        cJSON_Delete(item);
        return 0;
    }
    return 1;
}
