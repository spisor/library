#include "install_warning_report.h"
#include "allocation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_output.h"
#include "host_service.h"
#include "install_warnings.h"
#include "json_value.h"
#include "runtime/cache.h"
#include "runtime/install.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"

#define MANIFEST_FILE "plugin.json"

static int adds_version(cJSON *plugins, const char *id, size_t id_length, const char *version) {
    char *name = spisor_memory_allocate(id_length + 1);
    if (name == NULL) {
        return 0;
    }
    memcpy(name, id, id_length);
    name[id_length] = 0;
    cJSON_DeleteItemFromObjectCaseSensitive(plugins, name);
    int added = cJSON_AddStringToObject(plugins, name, version) != NULL;
    spisor_memory_release(name);
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

/** The {id, version} pairs of plugins, borrowing its keys and strings; freed, NULL when memory runs out. */
static runtime_item_version *item_versions_of(const cJSON *plugins, size_t *count) {
    size_t capacity = (size_t)cJSON_GetArraySize(plugins);
    runtime_item_version *versions = spisor_memory_allocate((capacity == 0 ? 1 : capacity) * sizeof *versions);
    const cJSON *listed = versions == NULL ? NULL : plugins;
    const cJSON *plugin;
    *count = 0;
    cJSON_ArrayForEach(plugin, listed) {
        versions[*count].id = plugin->string;
        versions[*count].version = plugin->valuestring;
        (*count)++;
    }
    return versions;
}

/** Finds the plugins among plugins that the cache holds a plugin.json for, as cache.locate answers them; 0 if not. */
static int locate_plugins(const host_context *context, const cJSON *plugins, runtime_cache_locate_answer *answer) {
    const runtime_cache_definition *cache = runtime_cache_definition_of();
    runtime_cache_locate_argument argument = SPISOR_INIT(runtime_cache_locate_argument);
    argument.cache = context->cache;
    runtime_item_version *versions = item_versions_of(plugins, &argument.plugin_count);
    argument.plugins = versions;
    int located = versions != NULL && cache->locate(cache->context, &argument, answer, NULL) == SPISOR_OK;
    spisor_memory_release(versions);
    return located;
}

/** The manifest at path, or NULL when it cannot be read as JSON, with *out_of_memory set when memory ran out. */
static cJSON *manifest_at(const char *path, int *out_of_memory) {
    size_t length = 0;
    spisor_read_outcome outcome = SPISOR_READ_FAILED;
    uint8_t *bytes = spisor_read_file_reporting(path, &length, &outcome);
    cJSON *manifest = bytes == NULL ? NULL : spisor_parse_json_reporting(bytes, length, out_of_memory);
    *out_of_memory = *out_of_memory || outcome == SPISOR_READ_OUT_OF_MEMORY;
    spisor_memory_release(bytes);
    return manifest;
}

static void print_line(const char *plugin_id, const char *subject, const char *detail) {
    char *safe_plugin = host_printable(plugin_id == NULL ? "" : plugin_id);
    char *safe_subject = subject == NULL ? NULL : host_printable(subject);
    host_write(1, "warning: ");
    host_write(1, safe_plugin == NULL ? "" : safe_plugin);
    host_write(1, ": ");
    if (safe_subject != NULL) {
        host_write(1, safe_subject);
        host_write(1, ": ");
    }
    host_write(1, detail);
    host_write(1, "\n");
    spisor_memory_release(safe_subject);
    spisor_memory_release(safe_plugin);
}

static void print_unchecked(const char *plugin_id) {
    print_line(plugin_id, NULL, "could not check this plugin's manifest");
}

static void print_warnings_of(const char *plugin_id, const cJSON *manifest) {
    const runtime_install_definition *install = runtime_install_definition_of();
    spisor_value_tree tree = SPISOR_INIT(spisor_value_tree);
    runtime_install_warnings_argument argument = SPISOR_INIT(runtime_install_warnings_argument);
    runtime_install_warnings_answer answer = SPISOR_INIT(runtime_install_warnings_answer);
    argument.manifest = &tree.root;
    if (spisor_value_of_json(manifest, &tree) != SPISOR_OK ||
        install->warnings(install->context, &argument, &answer, NULL) != SPISOR_OK) {
        print_unchecked(plugin_id);
    }
    for (size_t index = 0; index < answer.warning_count; index++) {
        print_line(plugin_id, answer.warnings[index].subject, answer.warnings[index].detail);
    }
    install->release_warnings_answer(install->context, &answer);
    spisor_value_release(&tree);
}

void host_print_install_warnings(const host_context *context, const cJSON *report) {
    const runtime_cache_definition *cache = runtime_cache_definition_of();
    runtime_cache_locate_answer located = SPISOR_INIT(runtime_cache_locate_answer);
    cJSON *plugins = reported_versions(report);
    if (plugins != NULL && locate_plugins(context, plugins, &located)) {
        for (size_t index = 0; index < located.located_count; index++) {
            const runtime_cache_located *item = &located.located[index];
            int out_of_memory = 0;
            cJSON *manifest = item->manifest == NULL ? NULL : manifest_at(item->manifest, &out_of_memory);
            if (out_of_memory) {
                break;
            }
            if (manifest == NULL) {
                print_unchecked(item->id);
            } else {
                print_warnings_of(item->id, manifest);
            }
            cJSON_Delete(manifest);
        }
        for (size_t index = 0; index < located.problem_count; index++) {
            const runtime_cache_damage *problem = &located.problems[index];
            if (problem->file != NULL && strcmp(problem->file, MANIFEST_FILE) == 0) {
                print_unchecked(problem->id);
            }
        }
    }
    cache->release_locate_answer(cache->context, &located);
    cJSON_Delete(plugins);
}
