#include "cache_refusal.h"

#include <stdlib.h>

#include "catalog_text.h"

char *runtime_cache_text(const char *const *parts, size_t count) {
    runtime_catalog_text text;
    runtime_catalog_text_start(&text);
    for (size_t index = 0; index < count; index++) {
        runtime_catalog_text_add(&text, parts[index]);
    }
    return runtime_catalog_text_finish(&text);
}

cJSON *runtime_cache_refusal(const char *const *parts, size_t count, const char *fix) {
    char *message = runtime_cache_text(parts, count);
    cJSON *refusal = message == NULL ? NULL : cJSON_CreateObject();
    if (refusal != NULL && (cJSON_AddStringToObject(refusal, "message", message) == NULL ||
                            cJSON_AddStringToObject(refusal, "fix", fix) == NULL)) {
        cJSON_Delete(refusal);
        refusal = NULL;
    }
    free(message);
    return refusal;
}
