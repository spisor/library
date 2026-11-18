#include "catalog_answer.h"

#include <stdio.h>

#include "json_service.h"

cJSON *runtime_catalog_refuse_member(const char *member, const char *shape, cJSON **refusal) {
    char message[256];
    char fix[512];
    snprintf(message, sizeof message, "the argument carries no usable %s", member);
    snprintf(fix, sizeof fix, "send %s as %s", member, shape);
    *refusal = runtime_refusal(message, fix);
    return NULL;
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
