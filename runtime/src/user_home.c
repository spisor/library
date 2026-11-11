#include "user_home.h"

#include <string.h>

#include "paths.h"

static char lowered(char letter) {
    return letter >= 'A' && letter <= 'Z' ? (char)(letter - 'A' + 'a') : letter;
}

int runtime_equal_ignoring_ascii_case(const char *left, const char *right) {
    while (*left != '\0' && lowered(*left) == lowered(*right)) {
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

const char *runtime_environment_variable(const cJSON *environment, const char *name, int windows) {
    const cJSON *entry;
    cJSON_ArrayForEach(entry, environment) {
        int matches = windows ? runtime_equal_ignoring_ascii_case(entry->string, name) : strcmp(entry->string, name) == 0;
        if (matches) {
            return cJSON_IsString(entry) ? entry->valuestring : NULL;
        }
    }
    return NULL;
}

const char *runtime_environment_text(const cJSON *environment, const char *name) {
    const cJSON *value = cJSON_IsObject(environment) ? cJSON_GetObjectItemCaseSensitive(environment, name) : NULL;
    return cJSON_IsString(value) ? value->valuestring : NULL;
}

const char *runtime_user_home_of(const cJSON *environment) {
    const char *home = RUNTIME_HOST_IS_WINDOWS ? runtime_environment_text(environment, "USERPROFILE") : NULL;
    home = home == NULL ? runtime_environment_text(environment, "HOME") : home;
    return home == NULL || home[0] == '\0' ? NULL : home;
}
