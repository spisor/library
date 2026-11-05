#include "install_settings.h"

#include <stdlib.h>
#include <string.h>

#include "cache_refusal.h"
#include "catalog.h"
#include "config.h"
#include "install_support.h"
#include "json_service.h"
#include "paths.h"
#include "spisor/path.h"

#define SOURCES_KEY "catalog.sources"

/**
 * @implNote Never created: settings read through it come from the portable layer alone, since the
 * settings functions always read some home.
 */
#define NO_HOME "portable-layer-only"

static cJSON *settings_call(const runtime_install_settings *settings, int application, int portable) {
    const char *segments[2] = {settings->state, NO_HOME};
    char *no_home = application ? NULL : spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
    cJSON *call = application || no_home != NULL ? cJSON_CreateObject() : NULL;
    if (call != NULL && (cJSON_AddStringToObject(call, "home", application ? settings->home : no_home) == NULL ||
                         (portable && cJSON_AddStringToObject(call, "portable", settings->portable) == NULL))) {
        cJSON_Delete(call);
        call = NULL;
    }
    free(no_home);
    return call;
}

cJSON *runtime_install_settings_values(const runtime_install_settings *settings, int application, int portable,
                                       cJSON **refusal) {
    cJSON *answer = runtime_call_or_refuse(runtime_settings_all, settings_call(settings, application, portable), refusal);
    cJSON *values = cJSON_DetachItemFromObjectCaseSensitive(answer, "values");
    cJSON_Delete(answer);
    return values;
}

int runtime_install_settings_set(const runtime_install_settings *settings, const char *key, cJSON *value,
                                 const char *layer, cJSON **refusal) {
    cJSON *call = settings_call(settings, 1, 1);
    if (call == NULL || cJSON_AddStringToObject(call, "key", key) == NULL || !cJSON_AddItemToObject(call, "value", value) ||
        cJSON_AddStringToObject(call, "layer", layer) == NULL) {
        if (call == NULL || cJSON_GetObjectItemCaseSensitive(call, "value") == NULL) {
            cJSON_Delete(value);
        }
        cJSON_Delete(call);
        return 0;
    }
    cJSON *answer = runtime_call_or_refuse(runtime_settings_set, call, refusal);
    cJSON_Delete(answer);
    return answer != NULL;
}

int runtime_install_plugin_enabled(const cJSON *values, const char *id) {
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(values, "plugins"), id), "enabled"));
}

char *runtime_install_enabled_key(const char *id) {
    const char *parts[3] = {"plugins.", id, ".enabled"};
    return runtime_cache_text(parts, 3);
}

int runtime_install_enable_plugin(const runtime_install_settings *settings, const char *id, const char *layer,
                                  cJSON **refusal) {
    char *key = runtime_install_enabled_key(id);
    int enabled = key != NULL && runtime_install_settings_set(settings, key, cJSON_CreateTrue(), layer, refusal);
    free(key);
    return enabled;
}

/** The catalog.sources list values carry, or NULL when they carry none. */
static const cJSON *listed_in(const cJSON *values) {
    const cJSON *sources = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(values, "catalog"), "sources");
    return cJSON_IsArray(sources) ? sources : NULL;
}

static int same_source(const cJSON *left, const cJSON *right) {
    const char *kind = runtime_install_text(left, "kind");
    const char *member = kind != NULL && strcmp(kind, "local") == 0 ? "path" : "url";
    const char *left_value = runtime_install_text(left, member);
    const char *right_value = runtime_install_text(right, member);
    return kind != NULL && runtime_install_text(right, "kind") != NULL &&
           strcmp(kind, runtime_install_text(right, "kind")) == 0 && left_value != NULL && right_value != NULL &&
           strcmp(left_value, right_value) == 0;
}

/** Whether entries, read as catalog.sources reads a list, names source; -1 when memory runs out. */
static int is_listed(const cJSON *entries, const cJSON *source) {
    cJSON *call = cJSON_Parse("{\"settings\": {\"catalog\": {}}}");
    cJSON *catalog = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(call, "settings"), "catalog");
    if (catalog == NULL || !runtime_install_copy_into(catalog, "sources", entries)) {
        cJSON_Delete(call);
        return -1;
    }
    cJSON *refusal = NULL;
    cJSON *read = runtime_call_or_refuse(runtime_catalog_sources, call, &refusal);
    cJSON_Delete(refusal);
    int listed = read == NULL ? -1 : 0;
    const cJSON *sources = cJSON_GetObjectItemCaseSensitive(read, "sources");
    for (const cJSON *listed_source = sources == NULL ? NULL : sources->child; listed == 0 && listed_source != NULL;
         listed_source = listed_source->next) {
        listed = same_source(listed_source, source);
    }
    cJSON_Delete(read);
    return listed;
}

static int append_to(const runtime_install_settings *settings, const char *layer, const cJSON *base, const cJSON *source,
                     cJSON **refusal) {
    int listed = is_listed(base, source);
    if (listed != 0) {
        return listed == 1;
    }
    cJSON *entries = cJSON_Duplicate(base, 1);
    cJSON *copy = cJSON_Duplicate(source, 1);
    if (entries == NULL || copy == NULL || !cJSON_AddItemToArray(entries, copy)) {
        cJSON_Delete(copy);
        cJSON_Delete(entries);
        return 0;
    }
    return runtime_install_settings_set(settings, SOURCES_KEY, entries, layer, refusal);
}

static int is_local(const cJSON *source) {
    const char *kind = runtime_install_text(source, "kind");
    return kind != NULL && strcmp(kind, "local") == 0;
}

/** Records one source given the merged list and each layer's own list (NULL when it has none). */
static int record_one(const runtime_install_settings *settings, const cJSON *source, const cJSON *merged,
                      const cJSON *application, const cJSON *portable, cJSON **refusal) {
    cJSON *empty = cJSON_CreateArray();
    int recorded = empty != NULL;
    if (recorded && is_local(source)) {
        recorded = append_to(settings, "application", application != NULL ? application : merged, source, refusal);
    } else if (recorded) {
        recorded = append_to(settings, "portable", portable != NULL ? portable : empty, source, refusal) &&
                   (application == NULL || append_to(settings, "application", application, source, refusal));
    }
    cJSON_Delete(empty);
    return recorded;
}

static int record_unlisted(const runtime_install_settings *settings, const cJSON *source, cJSON **refusal) {
    cJSON *merged = runtime_install_settings_values(settings, 1, 1, refusal);
    cJSON *application = merged == NULL ? NULL : runtime_install_settings_values(settings, 1, 0, refusal);
    cJSON *portable = application == NULL ? NULL : runtime_install_settings_values(settings, 0, 1, refusal);
    cJSON *empty = cJSON_CreateArray();
    const cJSON *merged_list = listed_in(merged);
    int listed = portable == NULL || empty == NULL ? -1 : is_listed(merged_list == NULL ? empty : merged_list, source);
    int recorded = listed == 1 || (listed == 0 && record_one(settings, source, merged_list == NULL ? empty : merged_list,
                                                             listed_in(application), listed_in(portable), refusal));
    cJSON_Delete(empty);
    cJSON_Delete(portable);
    cJSON_Delete(application);
    cJSON_Delete(merged);
    return recorded;
}

int runtime_install_record_sources(const runtime_install_settings *settings, const cJSON *sources, cJSON **refusal) {
    for (const cJSON *source = sources->child; source != NULL; source = source->next) {
        if (!record_unlisted(settings, source, refusal)) {
            return 0;
        }
    }
    return 1;
}
