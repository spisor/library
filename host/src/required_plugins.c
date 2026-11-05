#include "required_plugins.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache.h"
#include "cache_installed.h"
#include "config.h"
#include "host_output.h"
#include "host_service.h"
#include "install_settings.h"

static int is_located(const host_context *context, const char *id, const char *version) {
    cJSON *argument = cJSON_CreateObject();
    cJSON *plugins = argument == NULL ? NULL : cJSON_AddObjectToObject(argument, "plugins");
    if (plugins == NULL || cJSON_AddStringToObject(argument, "cache", context->cache) == NULL ||
        cJSON_AddStringToObject(plugins, id, version) == NULL) {
        cJSON_Delete(argument);
        return 0;
    }
    cJSON *refusal = NULL;
    cJSON *answer = host_call(runtime_cache_locate, argument, &refusal);
    int located = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(answer, "located")) > 0;
    cJSON_Delete(answer);
    cJSON_Delete(refusal);
    return located;
}

static int is_installed(const host_context *context, const cJSON *bundles, const char *id) {
    const cJSON *versions = cJSON_GetObjectItemCaseSensitive(bundles, id);
    for (const cJSON *version = versions == NULL ? NULL : versions->child; version != NULL; version = version->next) {
        if (cJSON_IsString(version) && is_located(context, id, version->valuestring)) {
            return 1;
        }
    }
    return 0;
}

static cJSON *refusal_with(const char *id, const char *loader_id, const char *state, const char *fix) {
    const char *parts[5] = {"the plugin ", id, " that ", loader_id, state};
    return host_refusal_of(parts, 5, fix);
}

static cJSON *not_installed(const char *id, const char *loader_id) {
    return refusal_with(id, loader_id, " requires is not installed", HOST_REINSTALL_FIX);
}

static cJSON *not_enabled(const host_context *context, const char *id, const char *loader_id,
                          const char *loader_home) {
    char *key = runtime_install_enabled_key(id);
    char *settings_file =
        key == NULL ? NULL : runtime_ecosystem_settings_file_holding(loader_home, context->portable, key);
    size_t size = settings_file == NULL ? 0 : strlen(key) + strlen(settings_file) + 32;
    char *fix = size == 0 ? NULL : malloc(size);
    if (fix != NULL) {
        snprintf(fix, size, "set %s to true in %s", key, settings_file);
    }
    cJSON *refusal = fix == NULL ? NULL : refusal_with(id, loader_id, " requires is disabled", fix);
    free(fix);
    free(settings_file);
    free(key);
    return refusal;
}

int host_check_required_plugins(const host_context *context, const char *loader_id, const char *loader_home,
                                const cJSON *standard, cJSON **refusal) {
    const cJSON *required = cJSON_GetObjectItemCaseSensitive(standard, "plugins");
    cJSON *bundles = runtime_cache_installed_bundles(context->cache);
    cJSON *settings = host_settings_of(context, loader_home, refusal);
    int fine = bundles != NULL && settings != NULL;
    for (const cJSON *id = fine && required != NULL ? required->child : NULL; fine && id != NULL; id = id->next) {
        if (!is_installed(context, bundles, id->valuestring)) {
            *refusal = not_installed(id->valuestring, loader_id);
            fine = 0;
        } else if (!runtime_install_plugin_enabled(settings, id->valuestring)) {
            *refusal = not_enabled(context, id->valuestring, loader_id, loader_home);
            fine = 0;
        }
    }
    cJSON_Delete(settings);
    cJSON_Delete(bundles);
    return fine;
}
