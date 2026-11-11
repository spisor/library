#include "places.h"

#include <stdlib.h>
#include <string.h>

#include "paths.h"
#include "spisor/json_kind.h"
#include "spisor/path.h"
#include "user_home.h"

static const char *usable_override(const cJSON *environment, const char *name, int windows) {
    const cJSON *entry = cJSON_GetObjectItemCaseSensitive(environment, name);
    if (!cJSON_IsString(entry) || entry->valuestring[0] == '\0') {
        return NULL;
    }
    return spisor_path_is_absolute(windows, entry->valuestring) ? entry->valuestring : NULL;
}

static char *join_two(int windows, const char *base, const char *name) {
    const char *segments[2] = {base, name};
    return spisor_path_join(windows, segments, 2);
}

static char *join_three(int windows, const char *base, const char *middle, const char *name) {
    const char *segments[3] = {base, middle, name};
    return spisor_path_join(windows, segments, 3);
}

static char *copy_text(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy != NULL) {
        memcpy(copy, text, size);
    }
    return copy;
}

static char *override_or(const char *override, char *fallback) {
    if (override == NULL) {
        return fallback;
    }
    free(fallback);
    return copy_text(override);
}

static char *linux_state(const char *user_home, const cJSON *environment) {
    const char *base = usable_override(environment, "XDG_STATE_HOME", 0);
    return base != NULL ? join_two(0, base, "spisor") : join_three(0, user_home, ".local/state", "spisor");
}

static char *linux_cache(const char *user_home, const cJSON *environment) {
    const char *base = usable_override(environment, "XDG_CACHE_HOME", 0);
    return base != NULL ? join_two(0, base, "spisor") : join_three(0, user_home, ".cache", "spisor");
}

static int resolve_defaults(int windows, int macos, const char *user_home, const cJSON *environment,
                            runtime_places *places) {
    if (windows) {
        const char *local_app_data = usable_override(environment, "LOCALAPPDATA", 1);
        int needs_default = usable_override(environment, "SPISOR_STATE", 1) == NULL ||
                            usable_override(environment, "SPISOR_CACHE", 1) == NULL;
        if (local_app_data == NULL && needs_default) {
            return RUNTIME_PLACES_NO_LOCAL_APP_DATA;
        }
        if (local_app_data != NULL) {
            places->state = join_two(1, local_app_data, "spisor");
            places->cache = join_three(1, local_app_data, "spisor", "cache");
        }
    } else if (macos) {
        places->state = join_three(0, user_home, "Library/Application Support", "spisor");
        places->cache = join_three(0, user_home, "Library/Caches", "spisor");
    } else {
        places->state = linux_state(user_home, environment);
        places->cache = linux_cache(user_home, environment);
    }
    places->portable = join_two(windows, user_home, ".spisor");
    return RUNTIME_PLACES_OK;
}

static void apply_overrides(int windows, const cJSON *environment, runtime_places *places) {
    places->portable = override_or(usable_override(environment, "SPISOR_HOME", windows), places->portable);
    places->state = override_or(usable_override(environment, "SPISOR_STATE", windows), places->state);
    places->cache = override_or(usable_override(environment, "SPISOR_CACHE", windows), places->cache);
}

static int place_secrets_file(int windows, const cJSON *environment, runtime_places *places) {
    const char *secrets_directory = usable_override(environment, "SPISOR_SECRETS", windows);
    places->force_file_store = secrets_directory != NULL;
    places->secrets_file =
        join_two(windows, secrets_directory != NULL ? secrets_directory : places->state, "secrets.json");
    return places->secrets_file != NULL;
}

void runtime_free_places(runtime_places *places) {
    free(places->portable);
    free(places->state);
    free(places->cache);
    free(places->secrets_file);
    memset(places, 0, sizeof(*places));
}

int runtime_resolve_places(int windows, int macos, const char *user_home, const cJSON *environment,
                           runtime_places *places) {
    memset(places, 0, sizeof(*places));
    int status = resolve_defaults(windows, macos, user_home, environment, places);
    if (status == RUNTIME_PLACES_OK) {
        apply_overrides(windows, environment, places);
        int complete = places->portable && places->state && places->cache &&
                       place_secrets_file(windows, environment, places);
        status = complete ? RUNTIME_PLACES_OK : RUNTIME_PLACES_OUT_OF_MEMORY;
    }
    if (status != RUNTIME_PLACES_OK) {
        runtime_free_places(places);
    }
    return status;
}

static int add_place(cJSON *result, const char *name, const char *path) {
    return cJSON_AddStringToObject(result, name, path) != NULL;
}

static cJSON *places_to_json(const char *home, const runtime_places *places) {
    cJSON *result = cJSON_CreateObject();
    int complete = result != NULL && add_place(result, "home", home) && add_place(result, "portable", places->portable) &&
                   add_place(result, "state", places->state) && add_place(result, "cache", places->cache) &&
                   add_place(result, "secretsFile", places->secrets_file) &&
                   cJSON_AddBoolToObject(result, "forceFileStore", places->force_file_store) != NULL;
    if (!complete) {
        cJSON_Delete(result);
        return NULL;
    }
    return result;
}

static int32_t out_of_memory(uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    return SPISOR_STATUS_OUT_OF_MEMORY;
}

static int32_t refuse_missing_local_app_data(uint8_t **out, size_t *out_len) {
    return spisor_fail("LOCALAPPDATA is absent, empty or not an absolute path",
                       "send the environment's LOCALAPPDATA as an absolute directory path; Windows has no default for "
                       "the machine state",
                       out, out_len);
}

static int32_t answer_places(const char *home, const cJSON *environment, uint8_t **out, size_t *out_len) {
    runtime_places places;
    int status = runtime_resolve_places(RUNTIME_HOST_IS_WINDOWS, RUNTIME_HOST_IS_MACOS, home, environment, &places);
    if (status == RUNTIME_PLACES_NO_LOCAL_APP_DATA) {
        return refuse_missing_local_app_data(out, out_len);
    }
    if (status != RUNTIME_PLACES_OK) {
        return out_of_memory(out, out_len);
    }
    cJSON *result = places_to_json(home, &places);
    runtime_free_places(&places);
    if (result == NULL) {
        return out_of_memory(out, out_len);
    }
    int32_t answer = spisor_succeed(result, out, out_len);
    cJSON_Delete(result);
    return answer;
}

RUNTIME_EXPORT int32_t runtime_places_resolve(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    spisor_call call;
    int32_t status = spisor_parse_call(in, in_len, 0, &call, out, out_len);
    if (status != SPISOR_STATUS_SUCCESS) {
        return status;
    }
    const cJSON *declared = cJSON_GetObjectItemCaseSensitive(call.argument, "home");
    const cJSON *environment = cJSON_GetObjectItemCaseSensitive(call.argument, "environment");
    const char *home = declared == NULL ? runtime_user_home_of(environment) : NULL;
    if (environment != NULL && !cJSON_IsObject(environment)) {
        status = spisor_fail("the argument's environment is not an object",
                             "send {\"environment\": {\"<name>\": \"<value>\"}}, or leave it out", out, out_len);
    } else if (declared == NULL && home == NULL) {
        status = spisor_fail(RUNTIME_NO_USER_HOME_MESSAGE, RUNTIME_NO_USER_HOME_FIX, out, out_len);
    } else if (declared != NULL && !cJSON_IsString(declared)) {
        status = spisor_fail("the argument's home is not a string", "send {\"home\": \"<directory>\"}, or leave it out",
                             out, out_len);
    } else {
        home = declared != NULL ? declared->valuestring : home;
        status = runtime_is_usable_home(home) ? answer_places(home, environment, out, out_len)
                                              : runtime_refuse_home(out, out_len);
    }
    spisor_discard_call(&call);
    return status;
}
