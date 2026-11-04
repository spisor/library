#include "user_home.h"

#include "paths.h"

const char *runtime_environment_text(const cJSON *environment, const char *name) {
    const cJSON *value = cJSON_IsObject(environment) ? cJSON_GetObjectItemCaseSensitive(environment, name) : NULL;
    return cJSON_IsString(value) ? value->valuestring : NULL;
}

const char *runtime_user_home_of(const cJSON *environment) {
    const char *home = RUNTIME_HOST_IS_WINDOWS ? runtime_environment_text(environment, "USERPROFILE") : NULL;
    home = home == NULL ? runtime_environment_text(environment, "HOME") : home;
    return home == NULL || home[0] == '\0' ? NULL : home;
}
