#include "host_service.h"

#include "cache_refusal.h"

const char *host_text(const cJSON *object, const char *name) {
    const cJSON *member = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(member) ? member->valuestring : NULL;
}

cJSON *host_refusal_of(const char *const *parts, size_t count, const char *fix) {
    return runtime_cache_refusal(parts, count, fix);
}
