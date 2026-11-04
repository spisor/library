#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "host_service.h"

#include "cache_refusal.h"
#include "json_service.h"

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

cJSON *host_call(runtime_json_function function, cJSON *argument, cJSON **refusal) {
    *refusal = NULL;
    return runtime_call_or_refuse(function, argument, refusal);
}

const char *host_text(const cJSON *object, const char *name) {
    const cJSON *member = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(member) ? member->valuestring : NULL;
}

cJSON *host_refusal_of(const char *const *parts, size_t count, const char *fix) {
    return runtime_cache_refusal(parts, count, fix);
}

#if defined(_WIN32)

int host_make_runnable(const char *path) {
    (void)path;
    return 1;
}

#else

int host_make_runnable(const char *path) {
    struct stat status;
    return stat(path, &status) == 0 && ((status.st_mode & S_IXUSR) != 0 || chmod(path, status.st_mode | S_IXUSR) == 0);
}

#endif
