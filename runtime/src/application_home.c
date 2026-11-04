#include "loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "followed_entry.h"
#include "json_service.h"
#include "paths.h"
#include "spisor/json_kind.h"
#include "spisor/path.h"
#include "user_home.h"

#define HOME_PREFIX "~/"
#define HOME_PREFIX_LENGTH 2

static const char *member_text(const cJSON *object, const char *name) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(value) ? value->valuestring : NULL;
}

/** The value environment holds for the variable home names under member; NULL when unnamed, unset or empty. */
static const char *variable_value(const cJSON *home, const char *member, const cJSON *environment) {
    const char *name = member_text(home, member);
    const char *value = name == NULL ? NULL : runtime_environment_text(environment, name);
    return value == NULL || value[0] == '\0' ? NULL : value;
}

static cJSON *answer_of(const char *home, const char *source) {
    cJSON *answer = cJSON_CreateObject();
    if (answer != NULL &&
        (cJSON_AddStringToObject(answer, "home", home) == NULL || cJSON_AddStringToObject(answer, "source", source) == NULL)) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static cJSON *relative_refusal(const char *name, const char *value) {
    size_t size = strlen(name) + strlen(value) + 64;
    char *message = malloc(size);
    char *fix = malloc(strlen(name) * 2 + 64);
    cJSON *refusal = NULL;
    if (message != NULL && fix != NULL) {
        snprintf(message, size, "%s is \"%s\", which is a relative path", name, value);
        snprintf(fix, strlen(name) * 2 + 64, "set %s to an absolute path, or unset it", name);
        refusal = runtime_refusal(message, fix);
    }
    free(message);
    free(fix);
    return refusal;
}

/** The answer of the variable home names under member, when it is set; NULL and *decided 0 otherwise. */
static cJSON *variable_home(const cJSON *home, const char *member, const char *source, const cJSON *environment,
                            int *decided, cJSON **refusal) {
    const char *value = variable_value(home, member, environment);
    *decided = value != NULL;
    if (value == NULL) {
        return NULL;
    }
    if (!spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, value)) {
        *refusal = relative_refusal(member_text(home, member), value);
        return NULL;
    }
    return answer_of(value, source);
}

static char *joined(const char *base, const char *rest) {
    const char *segments[2] = {base, rest};
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
}

static char *xdg_home(const cJSON *home, const cJSON *environment) {
    const char *subdirectory = member_text(home, "xdgSubdir");
    const char *config_home = runtime_environment_text(environment, "XDG_CONFIG_HOME");
    if (subdirectory == NULL || config_home == NULL || !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, config_home)) {
        return NULL;
    }
    return joined(config_home, subdirectory);
}

static char *expanded_candidate(const char *candidate, const char *user_home) {
    if (strncmp(candidate, HOME_PREFIX, HOME_PREFIX_LENGTH) != 0) {
        size_t length = strlen(candidate);
        char *copy = malloc(length + 1);
        if (copy != NULL) {
            memcpy(copy, candidate, length + 1);
        }
        return copy;
    }
    return joined(user_home, candidate + HOME_PREFIX_LENGTH);
}

static int needs_user_home(const cJSON *candidates) {
    const cJSON *candidate;
    cJSON_ArrayForEach(candidate, candidates) {
        if (strncmp(candidate->valuestring, HOME_PREFIX, HOME_PREFIX_LENGTH) == 0) {
            return 1;
        }
    }
    return 0;
}

static cJSON *candidate_home(const cJSON *candidates, const char *user_home) {
    char *first = NULL;
    cJSON *answer = NULL;
    int complete = 1;
    const cJSON *candidate;
    cJSON_ArrayForEach(candidate, candidates) {
        char *expanded = complete && answer == NULL ? expanded_candidate(candidate->valuestring, user_home) : NULL;
        complete = complete && (answer != NULL || expanded != NULL);
        if (expanded != NULL && runtime_is_followed_directory(expanded)) {
            answer = answer_of(expanded, "candidate");
            complete = answer != NULL;
        }
        if (first == NULL) {
            first = expanded;
        } else {
            free(expanded);
        }
    }
    if (answer == NULL && complete && first != NULL) {
        answer = answer_of(first, "default");
    }
    free(first);
    return answer;
}

static int is_candidate_list(const cJSON *candidates) {
    if (!cJSON_IsArray(candidates) || cJSON_GetArraySize(candidates) == 0) {
        return 0;
    }
    const cJSON *candidate;
    cJSON_ArrayForEach(candidate, candidates) {
        if (!cJSON_IsString(candidate)) {
            return 0;
        }
    }
    return 1;
}

static cJSON *fallback_home(const cJSON *home, const cJSON *environment, cJSON **refusal) {
    char *xdg = xdg_home(home, environment);
    if (xdg != NULL) {
        cJSON *answer = answer_of(xdg, "xdg");
        free(xdg);
        return answer;
    }
    const cJSON *candidates = cJSON_GetObjectItemCaseSensitive(home, "candidates");
    const char *user_home = runtime_user_home_of(environment);
    if (user_home == NULL && needs_user_home(candidates)) {
        *refusal = runtime_refusal(RUNTIME_NO_USER_HOME_MESSAGE, RUNTIME_NO_USER_HOME_FIX);
        return NULL;
    }
    return candidate_home(candidates, user_home);
}

static cJSON *refuse_home(void) {
    return runtime_refusal("the argument carries no usable home or environment",
                           "send {\"home\": {\"envOverride\"?, \"nativeEnv\"?, \"xdgSubdir\"?, \"candidates\": "
                           "[\"~/.<name>\"]}, \"environment\": {<name>: <value>}}, the home a loader standard declares");
}

cJSON *runtime_application_home(const cJSON *home, const cJSON *environment, cJSON **refusal) {
    if (!cJSON_IsObject(home) || !is_candidate_list(cJSON_GetObjectItemCaseSensitive(home, "candidates")) ||
        !cJSON_IsObject(environment)) {
        *refusal = refuse_home();
        return NULL;
    }
    int decided = 0;
    cJSON *answer = variable_home(home, "envOverride", "override", environment, &decided, refusal);
    if (!decided) {
        answer = variable_home(home, "nativeEnv", "native", environment, &decided, refusal);
    }
    return decided ? answer : fallback_home(home, environment, refusal);
}

static cJSON *home_for(const cJSON *argument, cJSON **refusal) {
    return runtime_application_home(cJSON_GetObjectItemCaseSensitive(argument, "home"),
                                    cJSON_GetObjectItemCaseSensitive(argument, "environment"), refusal);
}

RUNTIME_EXPORT int32_t runtime_loader_home(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, home_for);
}
