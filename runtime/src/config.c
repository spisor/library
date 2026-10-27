#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "merge.h"
#include "paths.h"
#include "segment.h"
#include "spisor/file.h"
#include "spisor/json_file.h"
#include "spisor/json_kind.h"
#include "spisor/json_write.h"
#include "spisor/path.h"

typedef struct config_target {
    char *directory;
    char *file;
} config_target;

typedef struct config_request {
    const char *home;
    const char *plugin;
    const cJSON *names;
    const cJSON *config;
    const cJSON *argument;
} config_request;

typedef enum locate_status { LOCATED, UNUSABLE_NAME, LOCATE_OUT_OF_MEMORY } locate_status;

typedef enum place_status { PLACED, CROSSES_NON_OBJECT, PLACE_OUT_OF_MEMORY } place_status;

typedef int32_t (*config_operation)(const config_request *request, uint8_t **out, size_t *out_len);

static const cJSON *optional_object(const cJSON *parent, const char *member) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, member);
    return cJSON_IsObject(item) ? item : NULL;
}

static char *copy_text(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy != NULL) {
        memcpy(copy, text, size);
    }
    return copy;
}

char *runtime_config_name(const char *plugin, const cJSON *config) {
    const cJSON *declared = cJSON_GetObjectItemCaseSensitive(config, "name");
    int declared_usable = cJSON_IsString(declared) && declared->valuestring[0] != '\0';
    return runtime_portable_segment(declared_usable ? declared->valuestring : plugin);
}

static void free_target(config_target *target) {
    free(target->directory);
    free(target->file);
    target->directory = NULL;
    target->file = NULL;
}

static char *file_name_of(const char *name) {
    size_t size = strlen(name) + sizeof(".json");
    char *file_name = malloc(size);
    if (file_name != NULL) {
        snprintf(file_name, size, "%s.json", name);
    }
    return file_name;
}

static locate_status join_target(const char *directory, const char *name, config_target *target) {
    char *file_name = file_name_of(name);
    const char *segments[2] = {directory, file_name};
    target->file = file_name != NULL ? spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2) : NULL;
    target->directory = copy_text(directory);
    free(file_name);
    if (target->file != NULL && target->directory != NULL) {
        return LOCATED;
    }
    free_target(target);
    return LOCATE_OUT_OF_MEMORY;
}

static locate_status locate_target(const config_request *request, config_target *target) {
    char *name = runtime_config_name(request->plugin, request->config);
    if (name == NULL) {
        return UNUSABLE_NAME;
    }
    runtime_paths paths;
    locate_status status = runtime_resolve_paths(request->home, request->names, &paths) == 0
                               ? join_target(paths.config, name, target)
                               : LOCATE_OUT_OF_MEMORY;
    runtime_free_paths(&paths);
    free(name);
    return status;
}

typedef struct settings_file {
    spisor_json_file_state state;
    cJSON *tree;
} settings_file;

static settings_file read_settings_file(const char *file_path) {
    settings_file file = {SPISOR_JSON_FILE_UNREADABLE, NULL};
    file.tree = spisor_read_json_file(file_path, &file.state);
    return file;
}

static int is_object_file(const settings_file *file) {
    return file->state == SPISOR_JSON_FILE_PARSED && cJSON_IsObject(file->tree);
}

static cJSON *merged_over_defaults(const config_request *request, const settings_file *file) {
    return runtime_merge(optional_object(request->config, "defaults"), is_object_file(file) ? file->tree : NULL);
}

static cJSON *merged_with_file(const config_request *request, const char *file_path) {
    settings_file file = read_settings_file(file_path);
    cJSON *merged = merged_over_defaults(request, &file);
    cJSON_Delete(file.tree);
    return merged;
}

cJSON *runtime_read_settings(const char *home, const cJSON *names, const char *plugin, const cJSON *config,
                             int *failed) {
    config_request request = {home, plugin, names, config, NULL};
    config_target target;
    cJSON *merged = NULL;
    if (locate_target(&request, &target) == LOCATED) {
        merged = merged_with_file(&request, target.file);
        free_target(&target);
    }
    *failed = merged == NULL;
    return merged;
}

static int32_t out_of_memory(uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    return SPISOR_STATUS_OUT_OF_MEMORY;
}

static int32_t refuse_name(uint8_t **out, size_t *out_len) {
    return spisor_fail("the plugin's config name is not a usable file name",
                       "name the plugin, or set config.name, to one portable path segment such as \"my-plugin\"", out,
                       out_len);
}

static int32_t locate_or_fail(const config_request *request, config_target *target, uint8_t **out, size_t *out_len) {
    locate_status status = locate_target(request, target);
    if (status == UNUSABLE_NAME) {
        return refuse_name(out, out_len);
    }
    return status == LOCATED ? SPISOR_STATUS_SUCCESS : out_of_memory(out, out_len);
}

static int32_t answer_member(const char *member, cJSON *value, uint8_t **out, size_t *out_len) {
    cJSON *result = cJSON_CreateObject();
    if (result == NULL || (value != NULL && !cJSON_AddItemToObject(result, member, value))) {
        cJSON_Delete(value);
        cJSON_Delete(result);
        return out_of_memory(out, out_len);
    }
    int32_t status = spisor_succeed(result, out, out_len);
    cJSON_Delete(result);
    return status;
}

static int has_empty_segment(const char *key) {
    return key[0] == '\0' || key[0] == '.' || key[strlen(key) - 1] == '.' || strstr(key, "..") != NULL;
}

static const char *key_of(const config_request *request) {
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(request->argument, "key");
    return cJSON_IsString(key) ? key->valuestring : NULL;
}

static int32_t refuse_key(uint8_t **out, size_t *out_len) {
    return spisor_fail("the argument carries no usable key",
                       "send {\"key\": \"a.b\"} with one or more dot separated, non-empty segments", out, out_len);
}

static int32_t config_all(const config_request *request, uint8_t **out, size_t *out_len) {
    config_target target;
    int32_t status = locate_or_fail(request, &target, out, out_len);
    if (status != SPISOR_STATUS_SUCCESS) {
        return status;
    }
    cJSON *merged = merged_with_file(request, target.file);
    free_target(&target);
    return merged == NULL ? out_of_memory(out, out_len) : answer_member("values", merged, out, out_len);
}

/** A copy of the value at key in merged, NULL when absent; *failed is set when memory ran out. */
static cJSON *copy_at(const cJSON *merged, const char *key, int *failed) {
    const cJSON *found = runtime_walk(merged, key);
    cJSON *copy = found == NULL ? NULL : cJSON_Duplicate(found, 1);
    *failed = found != NULL && copy == NULL;
    return copy;
}

static cJSON *value_at(const config_request *request, const config_target *target, const char *key, int *failed) {
    *failed = 0;
    if (has_empty_segment(key)) {
        return NULL;
    }
    cJSON *merged = merged_with_file(request, target->file);
    if (merged == NULL) {
        *failed = 1;
        return NULL;
    }
    cJSON *copy = copy_at(merged, key, failed);
    cJSON_Delete(merged);
    return copy;
}

static int32_t config_get(const config_request *request, uint8_t **out, size_t *out_len) {
    config_target target;
    int32_t status = locate_or_fail(request, &target, out, out_len);
    if (status != SPISOR_STATUS_SUCCESS) {
        return status;
    }
    const char *key = key_of(request);
    if (key == NULL) {
        free_target(&target);
        return refuse_key(out, out_len);
    }
    int failed;
    cJSON *found = value_at(request, &target, key, &failed);
    free_target(&target);
    return failed ? out_of_memory(out, out_len) : answer_member("value", found, out, out_len);
}

static int32_t refuse_file(const char *file_path, uint8_t **out, size_t *out_len) {
    char fix[2048];
    snprintf(fix, sizeof fix, "repair %s so it holds one JSON object, or remove it, then set again; it was left unchanged",
             file_path);
    return spisor_fail("the settings file is not a readable JSON object, so setting would discard its content", fix, out,
                       out_len);
}

static int32_t refuse_path(const char *file_path, const char *key, uint8_t **out, size_t *out_len) {
    char fix[2048];
    snprintf(fix, sizeof fix, "%s holds a value that is not an object on the way to \"%s\"; edit that file by hand",
             file_path, key);
    return spisor_fail("the key crosses a value in the settings file that is not an object", fix, out, out_len);
}

static cJSON *child_object_for_write(cJSON *node, const char *segment) {
    cJSON *child = cJSON_GetObjectItemCaseSensitive(node, segment);
    if (child == NULL) {
        child = cJSON_AddObjectToObject(node, segment);
    }
    return cJSON_IsObject(child) ? child : NULL;
}

static int attach_value(cJSON *node, const char *segment, const cJSON *value) {
    cJSON *copy = cJSON_Duplicate(value, 1);
    if (copy == NULL) {
        return 0;
    }
    int attached = cJSON_GetObjectItemCaseSensitive(node, segment) != NULL
                       ? cJSON_ReplaceItemInObjectCaseSensitive(node, segment, copy)
                       : cJSON_AddItemToObject(node, segment, copy);
    if (!attached) {
        cJSON_Delete(copy);
    }
    return attached;
}

static place_status place_value(cJSON *root, const char *key, const cJSON *value) {
    char *path = copy_text(key);
    if (path == NULL) {
        return PLACE_OUT_OF_MEMORY;
    }
    cJSON *node = root;
    char *segment = path;
    char *dot;
    while (node != NULL && (dot = strchr(segment, '.')) != NULL) {
        *dot = '\0';
        node = child_object_for_write(node, segment);
        segment = dot + 1;
    }
    place_status status = node == NULL            ? CROSSES_NON_OBJECT
                          : attach_value(node, segment, value) ? PLACED
                                                               : PLACE_OUT_OF_MEMORY;
    free(path);
    return status;
}

static int write_settings(const config_target *target, const cJSON *root) {
    char *text = spisor_json_stringify_pretty(root);
    if (text == NULL) {
        return 0;
    }
    size_t length = strlen(text);
    char *bytes = realloc(text, length + 2);
    if (bytes == NULL) {
        free(text);
        return 0;
    }
    bytes[length] = '\n';
    int written = spisor_make_directories(target->directory) &&
                  spisor_replace_file(target->file, (const uint8_t *)bytes, length + 1);
    free(bytes);
    return written;
}

/** The tree a set writes into, taking file's tree; NULL with *refused set when the file must not be replaced. */
static cJSON *writable_root(settings_file *file, int *refused) {
    cJSON *tree = file->tree;
    file->tree = NULL;
    *refused = 0;
    if (file->state == SPISOR_JSON_FILE_ABSENT) {
        return cJSON_CreateObject();
    }
    if (file->state == SPISOR_JSON_FILE_PARSED && cJSON_IsObject(tree)) {
        return tree;
    }
    cJSON_Delete(tree);
    *refused = 1;
    return NULL;
}

static int32_t refuse_write(const char *file_path, uint8_t **out, size_t *out_len) {
    char fix[2048];
    snprintf(fix, sizeof fix, "check that %s can be written, then set again", file_path);
    return spisor_fail("the settings file could not be written", fix, out, out_len);
}

static int32_t store_value(const config_target *target, settings_file *file, const char *key, const cJSON *value,
                           uint8_t **out, size_t *out_len) {
    int refused;
    cJSON *root = writable_root(file, &refused);
    if (root == NULL) {
        return refused ? refuse_file(target->file, out, out_len) : out_of_memory(out, out_len);
    }
    place_status placed = place_value(root, key, value);
    int32_t status = SPISOR_STATUS_SUCCESS;
    if (placed == CROSSES_NON_OBJECT) {
        status = refuse_path(target->file, key, out, out_len);
    } else if (placed == PLACE_OUT_OF_MEMORY) {
        status = out_of_memory(out, out_len);
    } else if (!write_settings(target, root)) {
        status = refuse_write(target->file, out, out_len);
    }
    cJSON_Delete(root);
    return status;
}

static int32_t refuse_missing_value(uint8_t **out, size_t *out_len) {
    return spisor_fail("the argument carries no value", "send {\"key\": \"a.b\", \"value\": <any JSON>}", out, out_len);
}

/** The merged value at key before the write, from the same read the write starts from. */
static cJSON *previous_value(const config_request *request, const settings_file *file, const char *key, int *failed) {
    cJSON *merged = merged_over_defaults(request, file);
    if (merged == NULL) {
        *failed = 1;
        return NULL;
    }
    cJSON *previous = copy_at(merged, key, failed);
    cJSON_Delete(merged);
    return previous;
}

static int32_t store_after_previous(const config_request *request, const config_target *target, const char *key,
                                    const cJSON *value, uint8_t **out, size_t *out_len) {
    settings_file file = read_settings_file(target->file);
    int failed;
    cJSON *previous = previous_value(request, &file, key, &failed);
    int32_t status = failed ? out_of_memory(out, out_len) : store_value(target, &file, key, value, out, out_len);
    cJSON_Delete(file.tree);
    if (status != SPISOR_STATUS_SUCCESS) {
        cJSON_Delete(previous);
        return status;
    }
    return answer_member("previous", previous, out, out_len);
}

static int32_t config_set(const config_request *request, uint8_t **out, size_t *out_len) {
    config_target target;
    int32_t status = locate_or_fail(request, &target, out, out_len);
    if (status != SPISOR_STATUS_SUCCESS) {
        return status;
    }
    const char *key = key_of(request);
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(request->argument, "value");
    if (key == NULL || has_empty_segment(key)) {
        status = refuse_key(out, out_len);
    } else if (value == NULL) {
        status = refuse_missing_value(out, out_len);
    } else {
        status = store_after_previous(request, &target, key, value, out, out_len);
    }
    free_target(&target);
    return status;
}

static int32_t dispatch(const uint8_t *in, size_t in_len, config_operation operation, uint8_t **out, size_t *out_len) {
    spisor_call call;
    int32_t status = spisor_parse_call(in, in_len, 0, &call, out, out_len);
    if (status != SPISOR_STATUS_SUCCESS) {
        return status;
    }
    const cJSON *home = cJSON_GetObjectItemCaseSensitive(call.argument, "home");
    const cJSON *plugin = cJSON_GetObjectItemCaseSensitive(call.argument, "plugin");
    if (!cJSON_IsString(home) || !cJSON_IsString(plugin)) {
        status = spisor_fail("the argument carries no string home and plugin",
                             "send {\"home\": \"<directory>\", \"plugin\": \"<manifest id>\"}", out, out_len);
    } else if (!runtime_is_usable_home(home->valuestring)) {
        status = runtime_refuse_home(out, out_len);
    } else {
        config_request request = {home->valuestring, plugin->valuestring, optional_object(call.argument, "names"),
                                  optional_object(call.argument, "config"), call.argument};
        status = operation(&request, out, out_len);
    }
    spisor_discard_call(&call);
    return status;
}

RUNTIME_EXPORT int32_t runtime_config_all(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return dispatch(in, in_len, config_all, out, out_len);
}

RUNTIME_EXPORT int32_t runtime_config_get(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return dispatch(in, in_len, config_get, out, out_len);
}

RUNTIME_EXPORT int32_t runtime_config_set(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return dispatch(in, in_len, config_set, out, out_len);
}
