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

#define PORTABLE_CONFIG_FILE "config.json"
#define ECOSYSTEM_SETTINGS_FILE "settings.json"

typedef struct config_target {
    char *directory;
    char *file;
} config_target;

typedef struct config_targets {
    config_target application;
    config_target portable;
} config_targets;

typedef struct config_request {
    const char *home;
    const char *portable;
    const char *plugin;
    const cJSON *names;
    const cJSON *config;
    const cJSON *argument;
} config_request;

typedef enum settings_layer { APPLICATION_LAYER, PORTABLE_LAYER, UNKNOWN_LAYER } settings_layer;

typedef enum locate_status { LOCATED, UNUSABLE_NAME, UNUSABLE_ID, LOCATE_OUT_OF_MEMORY } locate_status;

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

static void free_targets(config_targets *targets) {
    free_target(&targets->application);
    free_target(&targets->portable);
}

static char *file_name_of(const char *name) {
    size_t size = strlen(name) + sizeof(".json");
    char *file_name = malloc(size);
    if (file_name != NULL) {
        snprintf(file_name, size, "%s.json", name);
    }
    return file_name;
}

static locate_status join_target(const char *directory, const char *file_name, config_target *target) {
    const char *segments[2] = {directory, file_name};
    target->file = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
    target->directory = copy_text(directory);
    if (target->file != NULL && target->directory != NULL) {
        return LOCATED;
    }
    free_target(target);
    return LOCATE_OUT_OF_MEMORY;
}

static locate_status join_named_target(const char *directory, const char *name, config_target *target) {
    char *file_name = file_name_of(name);
    locate_status status = file_name != NULL ? join_target(directory, file_name, target) : LOCATE_OUT_OF_MEMORY;
    free(file_name);
    return status;
}

static locate_status locate_in_config_directory(const config_request *request, const char *name,
                                                config_target *target) {
    runtime_paths paths;
    locate_status status = runtime_resolve_paths(request->home, request->names, &paths) == 0
                               ? join_named_target(paths.config, name, target)
                               : LOCATE_OUT_OF_MEMORY;
    runtime_free_paths(&paths);
    return status;
}

static locate_status locate_application(const config_request *request, config_target *target) {
    char *name = runtime_config_name(request->plugin, request->config);
    if (name == NULL) {
        return UNUSABLE_NAME;
    }
    locate_status status = locate_in_config_directory(request, name, target);
    free(name);
    return status;
}

/** The plugin's file in the portable place, named by the manifest id and never by config.name (spec section 3). */
static locate_status locate_portable(const config_request *request, config_target *target) {
    if (request->portable == NULL) {
        return LOCATED;
    }
    char *folder_name = runtime_portable_segment(request->plugin);
    if (folder_name == NULL) {
        return UNUSABLE_ID;
    }
    char *folder = runtime_plugin_folder(request->portable, folder_name);
    locate_status status = folder != NULL ? join_target(folder, PORTABLE_CONFIG_FILE, target) : LOCATE_OUT_OF_MEMORY;
    free(folder);
    free(folder_name);
    return status;
}

static locate_status locate_plugin_targets(const config_request *request, config_targets *targets) {
    locate_status status = locate_application(request, &targets->application);
    if (status == LOCATED) {
        status = locate_portable(request, &targets->portable);
    }
    return status;
}

static locate_status locate_ecosystem_targets(const config_request *request, config_targets *targets) {
    locate_status status = locate_in_config_directory(request, "settings", &targets->application);
    if (status == LOCATED && request->portable != NULL) {
        status = join_target(request->portable, ECOSYSTEM_SETTINGS_FILE, &targets->portable);
    }
    return status;
}

static int is_ecosystem_request(const config_request *request) {
    return request->plugin == NULL;
}

static locate_status locate_targets(const config_request *request, config_targets *targets) {
    memset(targets, 0, sizeof(*targets));
    locate_status status = is_ecosystem_request(request) ? locate_ecosystem_targets(request, targets)
                                                         : locate_plugin_targets(request, targets);
    if (status != LOCATED) {
        free_targets(targets);
    }
    return status;
}

typedef struct settings_file {
    spisor_json_file_state state;
    cJSON *tree;
} settings_file;

typedef struct layer_files {
    settings_file portable;
    settings_file application;
} layer_files;

static settings_file read_settings_file(const char *file_path) {
    settings_file file = {SPISOR_JSON_FILE_ABSENT, NULL};
    if (file_path != NULL) {
        file.state = SPISOR_JSON_FILE_UNREADABLE;
        file.tree = spisor_read_json_file(file_path, &file.state);
    }
    return file;
}

static layer_files read_layers(const config_targets *targets) {
    layer_files files;
    files.portable = read_settings_file(targets->portable.file);
    files.application = read_settings_file(targets->application.file);
    return files;
}

static void free_layers(layer_files *files) {
    cJSON_Delete(files->portable.tree);
    cJSON_Delete(files->application.tree);
    files->portable.tree = NULL;
    files->application.tree = NULL;
}

static const cJSON *object_tree(const settings_file *file) {
    return file->state == SPISOR_JSON_FILE_PARSED && cJSON_IsObject(file->tree) ? file->tree : NULL;
}

/** defaults, then the portable layer, then the application layer, each merged over the last (spec section 4). */
static cJSON *merged_layers(const cJSON *defaults, const layer_files *files) {
    cJSON *under_application = runtime_merge(defaults, object_tree(&files->portable));
    if (under_application == NULL) {
        return NULL;
    }
    cJSON *merged = runtime_merge(under_application, object_tree(&files->application));
    cJSON_Delete(under_application);
    return merged;
}

static cJSON *merged_from_disk(const cJSON *defaults, const config_targets *targets) {
    layer_files files = read_layers(targets);
    cJSON *merged = merged_layers(defaults, &files);
    free_layers(&files);
    return merged;
}

static const cJSON *defaults_of(const config_request *request) {
    return optional_object(request->config, "defaults");
}

cJSON *runtime_read_settings(const char *home, const cJSON *names, const char *portable, const char *plugin,
                             const cJSON *config, int *failed) {
    config_request request = {home, portable, plugin, names, config, NULL};
    config_targets targets;
    cJSON *merged = NULL;
    if (locate_targets(&request, &targets) == LOCATED) {
        merged = merged_from_disk(defaults_of(&request), &targets);
        free_targets(&targets);
    }
    *failed = merged == NULL;
    return merged;
}

cJSON *runtime_read_ecosystem_settings(const char *home, const cJSON *names, const char *portable, int *failed) {
    config_request request = {home, portable, NULL, names, NULL, NULL};
    config_targets targets;
    cJSON *merged = NULL;
    if (locate_targets(&request, &targets) == LOCATED) {
        merged = merged_from_disk(NULL, &targets);
        free_targets(&targets);
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

static int32_t refuse_id(uint8_t **out, size_t *out_len) {
    return spisor_fail("the plugin id is not a usable folder name in the portable place",
                       "name the plugin by a manifest id that is one portable path segment such as \"my-plugin\"; "
                       "config.name does not name the portable folder",
                       out, out_len);
}

static int32_t locate_or_fail(const config_request *request, config_targets *targets, uint8_t **out,
                              size_t *out_len) {
    locate_status status = locate_targets(request, targets);
    if (status == UNUSABLE_NAME) {
        return refuse_name(out, out_len);
    }
    if (status == UNUSABLE_ID) {
        return refuse_id(out, out_len);
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
    config_targets targets;
    int32_t status = locate_or_fail(request, &targets, out, out_len);
    if (status != SPISOR_STATUS_SUCCESS) {
        return status;
    }
    cJSON *merged = merged_from_disk(defaults_of(request), &targets);
    free_targets(&targets);
    return merged == NULL ? out_of_memory(out, out_len) : answer_member("values", merged, out, out_len);
}

/** A copy of the value at key in merged, NULL when absent; *failed is set when memory ran out. */
static cJSON *copy_at(const cJSON *merged, const char *key, int *failed) {
    const cJSON *found = runtime_walk(merged, key);
    cJSON *copy = found == NULL ? NULL : cJSON_Duplicate(found, 1);
    *failed = found != NULL && copy == NULL;
    return copy;
}

static cJSON *value_at(const config_request *request, const config_targets *targets, const char *key, int *failed) {
    *failed = 0;
    if (has_empty_segment(key)) {
        return NULL;
    }
    cJSON *merged = merged_from_disk(defaults_of(request), targets);
    if (merged == NULL) {
        *failed = 1;
        return NULL;
    }
    cJSON *copy = copy_at(merged, key, failed);
    cJSON_Delete(merged);
    return copy;
}

static int32_t config_get(const config_request *request, uint8_t **out, size_t *out_len) {
    config_targets targets;
    int32_t status = locate_or_fail(request, &targets, out, out_len);
    if (status != SPISOR_STATUS_SUCCESS) {
        return status;
    }
    const char *key = key_of(request);
    if (key == NULL) {
        free_targets(&targets);
        return refuse_key(out, out_len);
    }
    int failed;
    cJSON *found = value_at(request, &targets, key, &failed);
    free_targets(&targets);
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

/** The merged value at key before the write, from the same reads the write starts from. */
static cJSON *previous_value(const config_request *request, const layer_files *files, const char *key, int *failed) {
    cJSON *merged = merged_layers(defaults_of(request), files);
    if (merged == NULL) {
        *failed = 1;
        return NULL;
    }
    cJSON *previous = copy_at(merged, key, failed);
    cJSON_Delete(merged);
    return previous;
}

typedef struct layer_write {
    const config_target *target;
    settings_file *file;
} layer_write;

static layer_write chosen_layer(settings_layer layer, const config_targets *targets, layer_files *files) {
    layer_write chosen = {&targets->application, &files->application};
    if (layer == PORTABLE_LAYER) {
        chosen.target = &targets->portable;
        chosen.file = &files->portable;
    }
    return chosen;
}

static int32_t store_after_previous(const config_request *request, const config_targets *targets, settings_layer layer,
                                    const char *key, const cJSON *value, uint8_t **out, size_t *out_len) {
    layer_files files = read_layers(targets);
    int failed;
    cJSON *previous = previous_value(request, &files, key, &failed);
    layer_write chosen = chosen_layer(layer, targets, &files);
    int32_t status = failed ? out_of_memory(out, out_len) : store_value(chosen.target, chosen.file, key, value, out, out_len);
    free_layers(&files);
    if (status != SPISOR_STATUS_SUCCESS) {
        cJSON_Delete(previous);
        return status;
    }
    return answer_member("previous", previous, out, out_len);
}

static settings_layer layer_of(const config_request *request) {
    const cJSON *layer = cJSON_GetObjectItemCaseSensitive(request->argument, "layer");
    if (layer == NULL) {
        return APPLICATION_LAYER;
    }
    if (!cJSON_IsString(layer)) {
        return UNKNOWN_LAYER;
    }
    return strcmp(layer->valuestring, "application") == 0 ? APPLICATION_LAYER
           : strcmp(layer->valuestring, "portable") == 0  ? PORTABLE_LAYER
                                                          : UNKNOWN_LAYER;
}

static int32_t refuse_layer(uint8_t **out, size_t *out_len) {
    return spisor_fail("the argument names no known layer",
                       "send layer as \"application\" (the default) or \"portable\"", out, out_len);
}

static int32_t refuse_missing_portable_layer(uint8_t **out, size_t *out_len) {
    return spisor_fail("this host has no portable settings layer",
                       "send portable as the portable answer of places.resolve, or set without layer \"portable\" to "
                       "write the application layer",
                       out, out_len);
}

static int32_t set_in_targets(const config_request *request, const config_targets *targets, settings_layer layer,
                              uint8_t **out, size_t *out_len) {
    const char *key = key_of(request);
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(request->argument, "value");
    if (key == NULL || has_empty_segment(key)) {
        return refuse_key(out, out_len);
    }
    if (value == NULL) {
        return refuse_missing_value(out, out_len);
    }
    return store_after_previous(request, targets, layer, key, value, out, out_len);
}

static int32_t config_set(const config_request *request, uint8_t **out, size_t *out_len) {
    settings_layer layer = layer_of(request);
    if (layer == UNKNOWN_LAYER) {
        return refuse_layer(out, out_len);
    }
    if (layer == PORTABLE_LAYER && request->portable == NULL) {
        return refuse_missing_portable_layer(out, out_len);
    }
    config_targets targets;
    int32_t status = locate_or_fail(request, &targets, out, out_len);
    if (status != SPISOR_STATUS_SUCCESS) {
        return status;
    }
    status = set_in_targets(request, &targets, layer, out, out_len);
    free_targets(&targets);
    return status;
}

static int32_t refuse_home_and_plugin(uint8_t **out, size_t *out_len) {
    return spisor_fail("the argument carries no string home and plugin",
                       "send {\"home\": \"<directory>\", \"plugin\": \"<manifest id>\"}", out, out_len);
}

static int32_t refuse_home(uint8_t **out, size_t *out_len) {
    return spisor_fail("the argument carries no string home", "send {\"home\": \"<directory>\"}", out, out_len);
}

static int32_t run_ecosystem_operation(const spisor_call *call, config_operation operation, uint8_t **out,
                                       size_t *out_len) {
    const cJSON *home = cJSON_GetObjectItemCaseSensitive(call->argument, "home");
    const char *portable;
    if (!cJSON_IsString(home)) {
        return refuse_home(out, out_len);
    }
    if (!runtime_is_usable_home(home->valuestring)) {
        return runtime_refuse_home(out, out_len);
    }
    if (!runtime_portable_argument(call->argument, &portable)) {
        return runtime_refuse_portable(out, out_len);
    }
    config_request request = {home->valuestring, portable, NULL, optional_object(call->argument, "names"), NULL,
                              call->argument};
    return operation(&request, out, out_len);
}

static int32_t run_operation(const spisor_call *call, config_operation operation, uint8_t **out, size_t *out_len) {
    const cJSON *home = cJSON_GetObjectItemCaseSensitive(call->argument, "home");
    const cJSON *plugin = cJSON_GetObjectItemCaseSensitive(call->argument, "plugin");
    const char *portable;
    if (!cJSON_IsString(home) || !cJSON_IsString(plugin)) {
        return refuse_home_and_plugin(out, out_len);
    }
    if (!runtime_is_usable_home(home->valuestring)) {
        return runtime_refuse_home(out, out_len);
    }
    if (!runtime_portable_argument(call->argument, &portable)) {
        return runtime_refuse_portable(out, out_len);
    }
    config_request request = {home->valuestring, portable, plugin->valuestring, optional_object(call->argument, "names"),
                              optional_object(call->argument, "config"), call->argument};
    return operation(&request, out, out_len);
}

typedef int32_t (*call_runner)(const spisor_call *call, config_operation operation, uint8_t **out, size_t *out_len);

static int32_t dispatch_with(call_runner runner, const uint8_t *in, size_t in_len, config_operation operation,
                             uint8_t **out, size_t *out_len) {
    spisor_call call;
    int32_t status = spisor_parse_call(in, in_len, 0, &call, out, out_len);
    if (status != SPISOR_STATUS_SUCCESS) {
        return status;
    }
    status = runner(&call, operation, out, out_len);
    spisor_discard_call(&call);
    return status;
}

static int32_t dispatch(const uint8_t *in, size_t in_len, config_operation operation, uint8_t **out, size_t *out_len) {
    return dispatch_with(run_operation, in, in_len, operation, out, out_len);
}

static int32_t dispatch_ecosystem(const uint8_t *in, size_t in_len, config_operation operation, uint8_t **out,
                                  size_t *out_len) {
    return dispatch_with(run_ecosystem_operation, in, in_len, operation, out, out_len);
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

RUNTIME_EXPORT int32_t runtime_settings_all(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return dispatch_ecosystem(in, in_len, config_all, out, out_len);
}

RUNTIME_EXPORT int32_t runtime_settings_set(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return dispatch_ecosystem(in, in_len, config_set, out, out_len);
}
