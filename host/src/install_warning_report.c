#include "install_warning_report.h"

#include <stdlib.h>
#include <string.h>

#include "cache.h"
#include "host_output.h"
#include "host_service.h"
#include "install_warnings.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"

static int adds_version(cJSON *plugins, const char *id, size_t id_length, const char *version) {
    char *name = malloc(id_length + 1);
    if (name == NULL) {
        return 0;
    }
    memcpy(name, id, id_length);
    name[id_length] = 0;
    cJSON_DeleteItemFromObjectCaseSensitive(plugins, name);
    int added = cJSON_AddStringToObject(plugins, name, version) != NULL;
    free(name);
    return added;
}

static int adds_installed_rows(cJSON *plugins, const cJSON *report) {
    const cJSON *row;
    cJSON_ArrayForEach(row, cJSON_GetObjectItemCaseSensitive(report, "installed")) {
        const char *id = host_text(row, "id");
        const char *version = host_text(row, "version");
        if (id != NULL && version != NULL && !adds_version(plugins, id, strlen(id), version)) {
            return 0;
        }
    }
    return 1;
}

/** The cached rows are "<id>@<version>", and a version holds no "@". */
static int adds_cached_rows(cJSON *plugins, const cJSON *report) {
    const cJSON *row;
    cJSON_ArrayForEach(row, cJSON_GetObjectItemCaseSensitive(report, "cached")) {
        const char *at = cJSON_IsString(row) ? strrchr(row->valuestring, '@') : NULL;
        if (at != NULL && !adds_version(plugins, row->valuestring, (size_t)(at - row->valuestring), at + 1)) {
            return 0;
        }
    }
    return 1;
}

/** The {id: version} of every item the report installed or found cached; NULL when memory runs out. */
static cJSON *reported_versions(const cJSON *report) {
    cJSON *plugins = cJSON_CreateObject();
    if (plugins == NULL || !adds_installed_rows(plugins, report) || !adds_cached_rows(plugins, report)) {
        cJSON_Delete(plugins);
        return NULL;
    }
    return plugins;
}

/** The plugins among plugins that the cache holds a plugin.json for, as cache.locate answers them. */
static cJSON *located_plugins(const host_context *context, cJSON *plugins) {
    cJSON *argument = cJSON_CreateObject();
    if (argument == NULL || cJSON_AddStringToObject(argument, "cache", context->cache) == NULL ||
        !cJSON_AddItemToObject(argument, "plugins", plugins)) {
        cJSON_Delete(argument);
        cJSON_Delete(plugins);
        return NULL;
    }
    cJSON *refusal = NULL;
    cJSON *answer = host_call(runtime_cache_locate, argument, &refusal);
    cJSON_Delete(refusal);
    return answer;
}

static cJSON *manifest_at(const char *path) {
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(path, &length);
    cJSON *manifest = bytes == NULL ? NULL : spisor_parse_json(bytes, length);
    free(bytes);
    return manifest;
}

static void print_warning(const char *plugin_id, const cJSON *warning) {
    const char *parts[] = {"warning: ", plugin_id, ": ", host_text(warning, "subject"), ": ", host_text(warning, "detail"), "\n"};
    for (size_t index = 0; index < sizeof parts / sizeof parts[0]; index++) {
        host_write(1, parts[index] == NULL ? "" : parts[index]);
    }
}

static void print_warnings_of(const char *plugin_id, cJSON *manifest) {
    cJSON *argument = cJSON_CreateObject();
    cJSON *refusal = NULL;
    cJSON *answer = NULL;
    if (argument != NULL && cJSON_AddItemToObject(argument, "manifest", manifest)) {
        answer = host_call(runtime_install_warnings, argument, &refusal);
    } else {
        cJSON_Delete(argument);
        cJSON_Delete(manifest);
    }
    const cJSON *warning;
    cJSON_ArrayForEach(warning, cJSON_GetObjectItemCaseSensitive(answer, "warnings")) {
        print_warning(plugin_id, warning);
    }
    cJSON_Delete(answer);
    cJSON_Delete(refusal);
}

void host_print_install_warnings(const host_context *context, const cJSON *report) {
    cJSON *plugins = reported_versions(report);
    cJSON *located = plugins == NULL ? NULL : located_plugins(context, plugins);
    const cJSON *item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(located, "located")) {
        const char *manifest_path = host_text(item, "manifest");
        cJSON *manifest = manifest_path == NULL ? NULL : manifest_at(manifest_path);
        if (manifest != NULL) {
            print_warnings_of(host_text(item, "id"), manifest);
        }
    }
    cJSON_Delete(located);
}
