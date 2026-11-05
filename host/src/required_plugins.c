#include "required_plugins.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache.h"
#include "cache_installed.h"
#include "config.h"
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

static cJSON *not_installed(const char *id, const char *loader_id, const char *application_id) {
    size_t size = strlen(application_id) + 96;
    char *fix = malloc(size);
    if (fix != NULL) {
        snprintf(fix, size, "forebay-host install --catalog <catalog> --application %s", application_id);
    }
    cJSON *refusal = fix == NULL ? NULL : refusal_with(id, loader_id, " requires is not installed", fix);
    free(fix);
    return refusal;
}

static cJSON *not_enabled(const char *id, const char *loader_id, const char *settings_file) {
    size_t size = strlen(id) + strlen(settings_file) + 64;
    char *fix = malloc(size);
    if (fix != NULL) {
        snprintf(fix, size, "set plugins.%s.enabled to true in %s", id, settings_file);
    }
    cJSON *refusal = fix == NULL ? NULL : refusal_with(id, loader_id, " requires is disabled", fix);
    free(fix);
    return refusal;
}

int host_check_required_plugins(const host_context *context, const char *loader_id, const char *loader_home,
                                const cJSON *standard, cJSON **refusal) {
    const cJSON *required = cJSON_GetObjectItemCaseSensitive(standard, "plugins");
    cJSON *bundles = runtime_cache_installed_bundles(context->cache);
    cJSON *settings = host_settings_of(context, loader_home, refusal);
    char *settings_file = runtime_ecosystem_settings_file(loader_home);
    const char *application_id = host_text(cJSON_GetObjectItemCaseSensitive(standard, "application"), "id");
    int fine = bundles != NULL && settings != NULL && settings_file != NULL && application_id != NULL;
    for (const cJSON *id = fine && required != NULL ? required->child : NULL; fine && id != NULL; id = id->next) {
        if (!is_installed(context, bundles, id->valuestring)) {
            *refusal = not_installed(id->valuestring, loader_id, application_id);
            fine = 0;
        } else if (!runtime_install_plugin_enabled(settings, id->valuestring)) {
            *refusal = not_enabled(id->valuestring, loader_id, settings_file);
            fine = 0;
        }
    }
    free(settings_file);
    cJSON_Delete(settings);
    cJSON_Delete(bundles);
    return fine;
}
