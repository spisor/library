#if defined(SPISOR_CONFIG_TEST_HOOKS) && !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "config.h"
#include "allocation.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "definition_support.h"
#include "hosts_lock.h"
#include "json_value.h"
#include "json_service.h"
#include "merge.h"
#include "paths.h"
#include "secrets.h"
#include "segment.h"
#include "spisor/file.h"
#include "spisor/json_file.h"
#include "spisor/json_write.h"
#include "spisor/path.h"

#ifdef SPISOR_CONFIG_TEST_HOOKS
#include "config_hooks.h"
#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif
#endif

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

typedef enum settings_layer { APPLICATION_LAYER, PORTABLE_LAYER } settings_layer;

typedef enum locate_status { LOCATED, UNUSABLE_NAME, UNUSABLE_ID, LOCATE_OUT_OF_MEMORY } locate_status;

typedef enum place_status { PLACED, CROSSES_NON_OBJECT, PLACE_OUT_OF_MEMORY } place_status;

static const cJSON *optional_object(const cJSON *parent, const char *member) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, member);
    return cJSON_IsObject(item) ? item : NULL;
}

static char *copy_text(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = spisor_memory_allocate(size);
    if (copy != NULL) {
        memcpy(copy, text, size);
    }
    return copy;
}

static const char *declared_or_plugin_name(const char *plugin, const cJSON *config) {
    const cJSON *declared = cJSON_GetObjectItemCaseSensitive(config, "name");
    int declared_usable = cJSON_IsString(declared) && declared->valuestring[0] != '\0';
    return declared_usable ? declared->valuestring : plugin;
}

spisor_status runtime_config_name(const char *plugin, const cJSON *config, char **name) {
    return runtime_portable_segment(declared_or_plugin_name(plugin, config), name);
}

static void free_target(config_target *target) {
    spisor_memory_release(target->directory);
    spisor_memory_release(target->file);
    target->directory = NULL;
    target->file = NULL;
}

static void free_targets(config_targets *targets) {
    free_target(&targets->application);
    free_target(&targets->portable);
}

static char *file_name_of(const char *name) {
    size_t size = strlen(name) + sizeof(".json");
    char *file_name = spisor_memory_allocate(size);
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
    spisor_memory_release(file_name);
    return status;
}

static locate_status locate_in_config_directory(const runtime_config_request *request, const char *name,
                                                config_target *target) {
    runtime_paths paths;
    locate_status status = runtime_resolve_paths(request->home, request->names, &paths) == 0
                               ? join_named_target(paths.config, name, target)
                               : LOCATE_OUT_OF_MEMORY;
    runtime_free_paths(&paths);
    return status;
}

static locate_status locate_application(const runtime_config_request *request, config_target *target) {
    char *name;
    if (runtime_config_name(request->plugin, request->config, &name) != SPISOR_OK) {
        return LOCATE_OUT_OF_MEMORY;
    }
    locate_status status = locate_in_config_directory(request, name, target);
    spisor_memory_release(name);
    return status;
}

/** The plugin's file in the portable place, named by the manifest id and never by config.name (spec section 3). */
static locate_status locate_portable(const runtime_config_request *request, config_target *target) {
    if (request->portable == NULL) {
        return LOCATED;
    }
    char *folder_name;
    if (runtime_portable_segment(request->plugin, &folder_name) != SPISOR_OK) {
        return LOCATE_OUT_OF_MEMORY;
    }
    char *folder = runtime_plugin_folder(request->portable, folder_name);
    locate_status status = folder != NULL ? join_target(folder, PORTABLE_CONFIG_FILE, target) : LOCATE_OUT_OF_MEMORY;
    spisor_memory_release(folder);
    spisor_memory_release(folder_name);
    return status;
}

static locate_status unusable_name_or_id(const runtime_config_request *request) {
    if (!runtime_trims_to_portable_segment(declared_or_plugin_name(request->plugin, request->config))) {
        return UNUSABLE_NAME;
    }
    return request->portable != NULL && !runtime_trims_to_portable_segment(request->plugin) ? UNUSABLE_ID : LOCATED;
}

static locate_status locate_plugin_targets(const runtime_config_request *request, config_targets *targets) {
    locate_status status = unusable_name_or_id(request);
    if (status != LOCATED) {
        return status;
    }
    status = locate_application(request, &targets->application);
    if (status == LOCATED) {
        status = locate_portable(request, &targets->portable);
    }
    return status;
}

static locate_status locate_ecosystem_targets(const runtime_config_request *request, config_targets *targets) {
    locate_status status = locate_in_config_directory(request, "settings", &targets->application);
    if (status == LOCATED && request->portable != NULL) {
        status = join_target(request->portable, ECOSYSTEM_SETTINGS_FILE, &targets->portable);
    }
    return status;
}

static int is_ecosystem_request(const runtime_config_request *request) {
    return request->plugin == NULL;
}

static locate_status locate_targets(const runtime_config_request *request, config_targets *targets) {
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

/** Whether a number below node is not finite, as a parsed 1e999 is, which no value tree can carry. */
static int holds_non_finite_number(const cJSON *node) {
    const cJSON *child;
    if (cJSON_IsNumber(node)) {
        return !isfinite(node->valuedouble);
    }
    cJSON_ArrayForEach(child, node) {
        if (holds_non_finite_number(child)) {
            return 1;
        }
    }
    return 0;
}

/**
 * One layer's file. A file holding a number no value tree carries reads as unreadable, so a read ignores it and a set
 * refuses to overwrite it, as it does any file that is not one readable JSON object.
 */
static settings_file read_settings_file(const char *file_path) {
    settings_file file = {SPISOR_JSON_FILE_ABSENT, NULL};
    if (file_path != NULL) {
        file.state = SPISOR_JSON_FILE_UNREADABLE;
        file.tree = spisor_read_json_file(file_path, &file.state);
    }
    if (file.state == SPISOR_JSON_FILE_PARSED && holds_non_finite_number(file.tree)) {
        cJSON_Delete(file.tree);
        file.tree = NULL;
        file.state = SPISOR_JSON_FILE_UNREADABLE;
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

static int ran_out_reading(const layer_files *files) {
    return files->portable.state == SPISOR_JSON_FILE_OUT_OF_MEMORY ||
           files->application.state == SPISOR_JSON_FILE_OUT_OF_MEMORY;
}

/** OK, or OUT_OF_MEMORY when a layer could not be read for want of it. */
static spisor_status check_layers(const layer_files *files) {
    return ran_out_reading(files) ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
}

/** defaults, then the portable layer, then the application layer, each merged over the last (spec section 4). */
static cJSON *merged_layers(const cJSON *defaults, const layer_files *files) {
    if (ran_out_reading(files)) {
        return NULL;
    }
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

/** The merged settings an operation answers from. */
static spisor_status merged_checked(const cJSON *defaults, const config_targets *targets, cJSON **merged) {
    layer_files files = read_layers(targets);
    spisor_status status = check_layers(&files);
    *merged = status == SPISOR_OK ? merged_layers(defaults, &files) : NULL;
    free_layers(&files);
    return status == SPISOR_OK && *merged == NULL ? SPISOR_OUT_OF_MEMORY : status;
}

static char *detached_file(config_target *target) {
    char *file = target->file;
    target->file = NULL;
    return file;
}

char *runtime_ecosystem_settings_file_holding(const char *home, const char *portable, const char *key) {
    runtime_config_request request = runtime_config_request_of(home, portable);
    config_targets targets;
    if (locate_targets(&request, &targets) != LOCATED) {
        return NULL;
    }
    layer_files files = read_layers(&targets);
    int ran_out = ran_out_reading(&files);
    int only_portable = runtime_walk(object_tree(&files.application), key) == NULL &&
                        runtime_walk(object_tree(&files.portable), key) != NULL;
    free_layers(&files);
    char *file = ran_out ? NULL : detached_file(only_portable ? &targets.portable : &targets.application);
    free_targets(&targets);
    return file;
}

runtime_config_request runtime_config_request_of(const char *home, const char *portable) {
    runtime_config_request request;
    memset(&request, 0, sizeof request);
    request.home = home;
    request.portable = portable;
    request.layer = RUNTIME_SETTINGS_LAYER_DEFAULT;
    return request;
}

static const cJSON *defaults_of(const runtime_config_request *request) {
    return optional_object(request->config, "defaults");
}

cJSON *runtime_read_settings(const char *home, const cJSON *names, const char *portable, const char *plugin,
                             const cJSON *config, int *failed) {
    runtime_config_request request = runtime_config_request_of(home, portable);
    request.plugin = plugin;
    request.names = names;
    request.config = config;
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
    runtime_config_request request = runtime_config_request_of(home, portable);
    request.names = names;
    config_targets targets;
    cJSON *merged = NULL;
    if (locate_targets(&request, &targets) == LOCATED) {
        merged = merged_from_disk(NULL, &targets);
        free_targets(&targets);
    }
    *failed = merged == NULL;
    return merged;
}

static spisor_status refuse_name(spisor_refusal *refusal) {
    return runtime_refuse_as(refusal, SPISOR_MALFORMED, "the plugin's config name is not a usable file name",
                             "name the plugin, or set config.name, to one portable path segment such as \"my-plugin\"");
}

static spisor_status refuse_id(spisor_refusal *refusal) {
    return runtime_refuse_as(refusal, SPISOR_MALFORMED,
                             "the plugin id is not a usable folder name in the portable place",
                             "name the plugin by a manifest id that is one portable path segment such as "
                             "\"my-plugin\"; config.name does not name the portable folder");
}

static spisor_status locate_or_refuse(const runtime_config_request *request, config_targets *targets,
                                      spisor_refusal *refusal) {
    locate_status status = locate_targets(request, targets);
    if (status == UNUSABLE_NAME) {
        return refuse_name(refusal);
    }
    if (status == UNUSABLE_ID) {
        return refuse_id(refusal);
    }
    return status == LOCATED ? SPISOR_OK : SPISOR_OUT_OF_MEMORY;
}

static int has_empty_segment(const char *key) {
    return key[0] == '\0' || key[0] == '.' || key[strlen(key) - 1] == '.' || strstr(key, "..") != NULL;
}

static spisor_status refuse_key(spisor_refusal *refusal) {
    return runtime_refuse_as(refusal, SPISOR_MALFORMED, "the argument carries no usable key",
                             "send key as a.b, one or more dot separated, non-empty segments");
}

static int is_secret_declaration(const cJSON *declaration) {
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(declaration, "type");
    return cJSON_IsString(type) && strcmp(type->valuestring, "secret") == 0;
}

static int is_secret_setting(const runtime_config_request *request, const char *key) {
    return key != NULL && is_secret_declaration(cJSON_GetObjectItemCaseSensitive(request->settings, key));
}

/** Removes the value at a dot path from tree; 0 when memory ran out. */
static int remove_at(cJSON *tree, const char *key) {
    const char *last_dot = strrchr(key, '.');
    if (last_dot == NULL) {
        if (cJSON_IsObject(tree)) {
            cJSON_DeleteItemFromObjectCaseSensitive(tree, key);
        }
        return 1;
    }
    char *parent_key = copy_text(key);
    if (parent_key == NULL) {
        return 0;
    }
    parent_key[last_dot - key] = '\0';
    cJSON *parent = (cJSON *)runtime_walk(tree, parent_key);
    if (cJSON_IsObject(parent)) {
        cJSON_DeleteItemFromObjectCaseSensitive(parent, last_dot + 1);
    }
    spisor_memory_release(parent_key);
    return 1;
}

/** The rest of path after key and a dot, or NULL when path does not lie below key. */
static const char *path_below(const char *path, const char *key) {
    size_t length = strlen(key);
    return strncmp(path, key, length) == 0 && path[length] == '.' ? path + length + 1 : NULL;
}

/** Removes from tree, the value at key or the whole tree when key is NULL, every declared secret below it. */
static int remove_secrets_below(const runtime_config_request *request, const char *key, cJSON *tree) {
    const cJSON *declaration;
    cJSON_ArrayForEach(declaration, request->settings) {
        const char *below = key == NULL ? declaration->string : path_below(declaration->string, key);
        if (below != NULL && is_secret_declaration(declaration) && !remove_at(tree, below)) {
            return 0;
        }
    }
    return 1;
}

/** The declared secret key that key lies inside, NULL when none. */
static const char *secret_above(const runtime_config_request *request, const char *key) {
    const cJSON *declaration;
    cJSON_ArrayForEach(declaration, request->settings) {
        if (is_secret_declaration(declaration) && path_below(key, declaration->string) != NULL) {
            return declaration->string;
        }
    }
    return NULL;
}

/** The declared secret key a set of value at key would write into, NULL when none. */
static const char *secret_reached(const runtime_config_request *request, const char *key, const cJSON *value) {
    const char *above = secret_above(request, key);
    const cJSON *declaration;
    if (above != NULL) {
        return above;
    }
    cJSON_ArrayForEach(declaration, request->settings) {
        const char *below = path_below(declaration->string, key);
        if (below != NULL && is_secret_declaration(declaration) && runtime_walk(value, below) != NULL) {
            return declaration->string;
        }
    }
    return NULL;
}

/** Whether the request names a home and portable place it may read and write under; the refusal otherwise. */
static spisor_status check_places(const runtime_config_request *request, spisor_refusal *refusal) {
    if (!runtime_is_usable_home(request->home)) {
        return runtime_refuse_home(refusal);
    }
    if (request->portable != NULL && !runtime_is_usable_home(request->portable)) {
        return runtime_refuse_portable(refusal);
    }
    return SPISOR_OK;
}

spisor_status runtime_config_all_values(const runtime_config_request *request, cJSON **values, spisor_refusal *refusal) {
    config_targets targets;
    spisor_status status = check_places(request, refusal);
    *values = NULL;
    if (status == SPISOR_OK) {
        status = locate_or_refuse(request, &targets, refusal);
    }
    if (status != SPISOR_OK) {
        return status;
    }
    cJSON *merged = NULL;
    status = merged_checked(defaults_of(request), &targets, &merged);
    free_targets(&targets);
    if (status == SPISOR_OK && !remove_secrets_below(request, NULL, merged)) {
        status = SPISOR_OUT_OF_MEMORY;
    }
    if (status != SPISOR_OK) {
        cJSON_Delete(merged);
        return status;
    }
    *values = merged;
    return SPISOR_OK;
}

/**
 * A copy of the value at key in merged without the declared secrets below it, NULL when absent or
 * inside a declared secret; *failed is set when memory ran out.
 */
static cJSON *copy_at(const runtime_config_request *request, const cJSON *merged, const char *key, int *failed) {
    const cJSON *found = secret_above(request, key) == NULL ? runtime_walk(merged, key) : NULL;
    cJSON *copy = found == NULL ? NULL : cJSON_Duplicate(found, 1);
    *failed = found != NULL && copy == NULL;
    if (copy != NULL && !remove_secrets_below(request, key, copy)) {
        cJSON_Delete(copy);
        copy = NULL;
        *failed = 1;
    }
    return copy;
}

static spisor_status value_at(const runtime_config_request *request, const config_targets *targets, cJSON **value) {
    cJSON *merged = NULL;
    int failed = 0;
    spisor_status status = merged_checked(defaults_of(request), targets, &merged);
    if (status != SPISOR_OK || has_empty_segment(request->key)) {
        cJSON_Delete(merged);
        return status == SPISOR_OK ? SPISOR_NONE : status;
    }
    *value = copy_at(request, merged, request->key, &failed);
    cJSON_Delete(merged);
    if (failed) {
        return SPISOR_OUT_OF_MEMORY;
    }
    return *value == NULL ? SPISOR_NONE : SPISOR_OK;
}

static spisor_status get_secret_setting(const runtime_config_request *request, cJSON **value, spisor_refusal *refusal) {
    char *text = NULL;
    spisor_status status;
    if (request->places == NULL) {
        return SPISOR_NONE;
    }
    status = runtime_get_secret(request->places, request->plugin, request->key, &text, refusal);
    if (status == SPISOR_OK) {
        *value = cJSON_CreateString(text);
        status = *value == NULL ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
        runtime_overwrite(text, strlen(text));
    }
    spisor_memory_release(text);
    return status;
}

spisor_status runtime_config_get_value(const runtime_config_request *request, cJSON **value, spisor_refusal *refusal) {
    config_targets targets;
    spisor_status status = check_places(request, refusal);
    *value = NULL;
    if (status != SPISOR_OK) {
        return status;
    }
    if (is_secret_setting(request, request->key)) {
        return get_secret_setting(request, value, refusal);
    }
    status = locate_or_refuse(request, &targets, refusal);
    if (status != SPISOR_OK) {
        return status;
    }
    status = request->key == NULL ? refuse_key(refusal) : value_at(request, &targets, value);
    free_targets(&targets);
    return status;
}

static spisor_status refuse_file(const char *file_path, spisor_refusal *refusal) {
    char fix[2048];
    snprintf(fix, sizeof fix, "repair %s so it holds one JSON object, or remove it, then set again; it was left unchanged",
             file_path);
    return runtime_refuse_as(refusal, SPISOR_REFUSED,
                             "the settings file is not a readable JSON object, so setting would discard its content",
                             fix);
}

static spisor_status refuse_path(const char *file_path, const char *key, spisor_refusal *refusal) {
    char fix[2048];
    snprintf(fix, sizeof fix, "%s holds a value that is not an object on the way to \"%s\"; edit that file by hand",
             file_path, key);
    return runtime_refuse_as(refusal, SPISOR_REFUSED, "the key crosses a value in the settings file that is not an object",
                             fix);
}

static cJSON *child_object_for_write(cJSON *node, const char *segment, int *out_of_memory) {
    cJSON *child = cJSON_GetObjectItemCaseSensitive(node, segment);
    if (child == NULL) {
        child = cJSON_AddObjectToObject(node, segment);
        *out_of_memory = child == NULL;
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
    int out_of_memory = 0;
    while (node != NULL && (dot = strchr(segment, '.')) != NULL) {
        *dot = '\0';
        node = child_object_for_write(node, segment, &out_of_memory);
        segment = dot + 1;
    }
    place_status status = out_of_memory           ? PLACE_OUT_OF_MEMORY
                          : node == NULL          ? CROSSES_NON_OBJECT
                          : attach_value(node, segment, value) ? PLACED
                                                               : PLACE_OUT_OF_MEMORY;
    spisor_memory_release(path);
    return status;
}

static int write_settings(const config_target *target, const cJSON *root) {
    char *text = spisor_json_stringify_pretty(root);
    if (text == NULL) {
        return 0;
    }
    size_t length = strlen(text);
    char *bytes = spisor_memory_reallocate(text, length + 2);
    if (bytes == NULL) {
        spisor_memory_release(text);
        return 0;
    }
    bytes[length] = '\n';
    int written = spisor_make_directories(target->directory) &&
                  spisor_replace_file(target->file, (const uint8_t *)bytes, length + 1);
    spisor_memory_release(bytes);
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

static spisor_status refuse_write(const char *file_path, spisor_refusal *refusal) {
    char fix[2048];
    snprintf(fix, sizeof fix, "check that %s can be written, then set again", file_path);
    return runtime_refuse_as(refusal, SPISOR_REFUSED, "the settings file could not be written", fix);
}

static place_status place_request(cJSON *root, const runtime_config_request *request) {
    cJSON *merged = request->merges ? runtime_merge(runtime_walk(root, request->key), request->value) : NULL;
    const cJSON *value = request->merges ? merged : request->value;
    place_status placed = value == NULL ? PLACE_OUT_OF_MEMORY : place_value(root, request->key, value);
    cJSON_Delete(merged);
    return placed;
}

static spisor_status store_value(const config_target *target, settings_file *file,
                                 const runtime_config_request *request, spisor_refusal *refusal) {
    int refused;
    cJSON *root = writable_root(file, &refused);
    if (root == NULL) {
        return refused ? refuse_file(target->file, refusal) : SPISOR_OUT_OF_MEMORY;
    }
    place_status placed = place_request(root, request);
    spisor_status status = SPISOR_OK;
    if (placed == CROSSES_NON_OBJECT) {
        status = refuse_path(target->file, request->key, refusal);
    } else if (placed == PLACE_OUT_OF_MEMORY) {
        status = SPISOR_OUT_OF_MEMORY;
    } else if (!write_settings(target, root)) {
        status = refuse_write(target->file, refusal);
    }
    cJSON_Delete(root);
    return status;
}

static spisor_status refuse_missing_value(spisor_refusal *refusal) {
    return runtime_refuse_as(refusal, SPISOR_MALFORMED, "the argument carries no value",
                             "send value as the setting's value, any value but one of kind NONE");
}

/** The merged value at key before the write, from the same reads the write starts from. */
static cJSON *previous_value(const runtime_config_request *request, const layer_files *files, const char *key, int *failed) {
    cJSON *merged = merged_layers(defaults_of(request), files);
    if (merged == NULL) {
        *failed = 1;
        return NULL;
    }
    cJSON *previous = copy_at(request, merged, key, failed);
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

#ifdef SPISOR_CONFIG_TEST_HOOKS
#if defined(_WIN32)
#define pause_briefly() Sleep(20)
#else
static void pause_briefly(void) {
    struct timespec pause = {0, 20 * 1000000L};
    nanosleep(&pause, NULL);
}
#endif

static char *pause_signals = NULL;

void runtime_config_pause_after_read(const char *signals) {
    spisor_memory_release(pause_signals);
    pause_signals = signals == NULL ? NULL : spisor_path_join(RUNTIME_HOST_IS_WINDOWS, &signals, 1);
}

/** Pauses the first writer to arrive, once, until the test releases it. */
static void pause_after_read(void) {
    char *signals = pause_signals;
    pause_signals = NULL;
    if (signals == NULL) {
        return;
    }
    const char *paused_segments[2] = {signals, "paused"};
    const char *release_segments[2] = {signals, "release"};
    char *paused = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, paused_segments, 2);
    char *release = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, release_segments, 2);
    spisor_replace_file(paused, (const uint8_t *)"", 0);
    for (int waited = 0; waited < 500 && spisor_probe_file(release) != SPISOR_FILE_PRESENT; waited++) {
        pause_briefly();
    }
    spisor_memory_release(release);
    spisor_memory_release(paused);
    spisor_memory_release(signals);
}
#else
static void pause_after_read(void) {
}
#endif

/** The previous value as the tree a definition answers, built before the write so a set that refuses never wrote. */
static spisor_status previous_tree(const runtime_config_request *request, const cJSON *previous,
                                   spisor_refusal *refusal) {
    spisor_status status;
    if (request->previous_tree == NULL || previous == NULL) {
        return SPISOR_OK;
    }
    status = spisor_value_of_json(previous, request->previous_tree);
    if (status == SPISOR_MALFORMED) {
        return runtime_refuse_as(refusal, SPISOR_REFUSED, "the settings hold what no value tree can carry",
                                 "repair the settings files, then try again");
    }
    return status;
}

/** The previous value, read from the layers the write starts from; nothing is written unless it answers OK. */
static spisor_status read_previous(const runtime_config_request *request, const layer_files *files, cJSON **previous,
                                   spisor_refusal *refusal) {
    int failed = 0;
    spisor_status status = check_layers(files);
    if (status == SPISOR_OK) {
        *previous = previous_value(request, files, request->key, &failed);
        status = failed ? SPISOR_OUT_OF_MEMORY : previous_tree(request, *previous, refusal);
    }
    return status;
}

static spisor_status store_locked(const runtime_config_request *request, const config_targets *targets, settings_layer layer,
                                  cJSON **previous, spisor_refusal *refusal) {
    layer_files files = read_layers(targets);
    pause_after_read();
    spisor_status status = read_previous(request, &files, previous, refusal);
    layer_write chosen = chosen_layer(layer, targets, &files);
    if (status == SPISOR_OK) {
        status = store_value(chosen.target, chosen.file, request, refusal);
    }
    free_layers(&files);
    if (status != SPISOR_OK) {
        cJSON_Delete(*previous);
        *previous = NULL;
        spisor_value_release(request->previous_tree);
    }
    return status;
}

/**
 * Locks the settings file of the layer written, in its own folder, so two writers of different keys never
 * lose one update, in *guard: SPISOR_REFUSED when the lock cannot be taken, SPISOR_OUT_OF_MEMORY when memory runs out.
 *
 * @implNote Its own lock file, never the registry lock, so a caller holding that may still write settings;
 * only the read and the write of one file happen under it.
 */
static spisor_status lock_layer(const config_target *target, spisor_lock **guard) {
    const char *segments[2] = {target->directory, ".settings.lock"};
    *guard = NULL;
    if (!spisor_make_directories(target->directory)) {
        return SPISOR_REFUSED;
    }
    char *path = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
    spisor_status status = path == NULL ? SPISOR_OUT_OF_MEMORY : spisor_lock_file(path, guard);
    spisor_memory_release(path);
    return status;
}

static spisor_status store_after_previous(const runtime_config_request *request, const config_targets *targets,
                                          settings_layer layer, cJSON **previous, spisor_refusal *refusal) {
    spisor_lock *guard;
    spisor_status locked = lock_layer(layer == PORTABLE_LAYER ? &targets->portable : &targets->application, &guard);
    if (locked == SPISOR_OUT_OF_MEMORY) {
        return SPISOR_OUT_OF_MEMORY;
    }
    if (locked != SPISOR_OK) {
        return runtime_refuse_as(refusal, SPISOR_REFUSED, "the settings folder could not be locked for writing",
                                 "check that the settings folder is writable, then try again");
    }
    spisor_status status = store_locked(request, targets, layer, previous, refusal);
    spisor_unlock(guard);
    return status;
}

/** A declared key takes its declared layer, or portable when it declares none; an undeclared key the application. */
static settings_layer default_layer(const runtime_config_request *request) {
    const cJSON *declaration = cJSON_GetObjectItemCaseSensitive(request->settings, request->key);
    const cJSON *layer = cJSON_GetObjectItemCaseSensitive(declaration, "layer");
    if (declaration == NULL) {
        return APPLICATION_LAYER;
    }
    return cJSON_IsString(layer) && strcmp(layer->valuestring, "application") == 0 ? APPLICATION_LAYER
                                                                                    : PORTABLE_LAYER;
}

static settings_layer layer_of(const runtime_config_request *request) {
    if (request->layer == RUNTIME_SETTINGS_LAYER_APPLICATION) {
        return APPLICATION_LAYER;
    }
    return request->layer == RUNTIME_SETTINGS_LAYER_PORTABLE ? PORTABLE_LAYER : default_layer(request);
}

static spisor_status refuse_missing_portable_layer(spisor_refusal *refusal) {
    return runtime_refuse_as(refusal, SPISOR_MALFORMED, "this host has no portable settings layer",
                             "send portable as the portable answer of places' resolve, or set without the portable "
                             "layer to write the application layer");
}

static spisor_status refuse_reaching_secret(const char *key, const char *secret, spisor_refusal *refusal) {
    char detail[4096];
    snprintf(detail, sizeof detail, "config.set(\"%s\"): %s is a secret setting; set it on its own", key, secret);
    return runtime_refuse_as(refusal, SPISOR_REFUSED, detail, "set the secret setting by its own key");
}

static spisor_status set_in_targets(const runtime_config_request *request, const config_targets *targets, settings_layer layer,
                                    cJSON **previous, spisor_refusal *refusal) {
    const char *secret;
    if (request->key == NULL || has_empty_segment(request->key)) {
        return refuse_key(refusal);
    }
    if (request->value == NULL) {
        return refuse_missing_value(refusal);
    }
    secret = secret_reached(request, request->key, request->value);
    if (secret != NULL) {
        return refuse_reaching_secret(request->key, secret, refusal);
    }
    return store_after_previous(request, targets, layer, previous, refusal);
}

static spisor_status refuse_secret_without_store(const char *key, spisor_refusal *refusal) {
    char detail[2048];
    snprintf(detail, sizeof detail, "config.set(\"%s\"): the setting is a secret and this host has no secret store",
             key);
    return runtime_refuse_as(refusal, SPISOR_REFUSED, detail, "start the host with a secret store");
}

static spisor_status refuse_secret_value(const char *key, spisor_refusal *refusal) {
    char detail[2048];
    snprintf(detail, sizeof detail, "config.set(\"%s\"): a secret setting takes text or null", key);
    return runtime_refuse_as(refusal, SPISOR_REFUSED, detail, "send the value as text, or null to remove the secret");
}

/**
 * Stores a setting the plugin declares secret through the secrets service, never in a settings file.
 *
 * @implNote The previous value is never answered, so a secret cannot leave through the answer of a set.
 */
static spisor_status set_secret_setting(const runtime_config_request *request, int *secret, spisor_refusal *refusal) {
    runtime_secret_location location;
    spisor_status status;
    if (request->value == NULL) {
        return refuse_missing_value(refusal);
    }
    if (request->places == NULL) {
        return refuse_secret_without_store(request->key, refusal);
    }
    if (cJSON_IsNull(request->value)) {
        status = runtime_delete_secret(request->places, request->plugin, request->key, refusal);
    } else if (cJSON_IsString(request->value)) {
        status = runtime_set_secret(request->places, request->plugin, request->key, request->value->valuestring,
                                    &location, refusal);
    } else {
        return refuse_secret_value(request->key, refusal);
    }
    *secret = status == SPISOR_OK;
    return status;
}

spisor_status runtime_config_set_value(const runtime_config_request *request, cJSON **previous, int *secret,
                                       spisor_refusal *refusal) {
    config_targets targets;
    settings_layer layer = layer_of(request);
    spisor_status status = check_places(request, refusal);
    *previous = NULL;
    *secret = 0;
    if (status != SPISOR_OK) {
        return status;
    }
    if (is_secret_setting(request, request->key)) {
        return set_secret_setting(request, secret, refusal);
    }
    if (layer == PORTABLE_LAYER && request->portable == NULL) {
        return refuse_missing_portable_layer(refusal);
    }
    status = locate_or_refuse(request, &targets, refusal);
    if (status != SPISOR_OK) {
        return status;
    }
    status = set_in_targets(request, &targets, layer, previous, refusal);
    free_targets(&targets);
    return status;
}

/** The refusal of a status that is not OK, as runtime's JSON callers hold one; NULL when memory ran out. */
static cJSON *refusal_json(spisor_status status, spisor_refusal *refusal) {
    cJSON *refused = status == SPISOR_OK ? NULL : runtime_refusal_of(refusal);
    spisor_refusal_release(refusal);
    return refused;
}

cJSON *runtime_ecosystem_settings_values(const char *home, const char *portable, cJSON **refusal) {
    runtime_config_request request = runtime_config_request_of(home, portable);
    spisor_refusal refused = SPISOR_INIT(spisor_refusal);
    cJSON *values = NULL;
    *refusal = refusal_json(runtime_config_all_values(&request, &values, &refused), &refused);
    return values;
}

static int set_ecosystem_setting(runtime_config_request *request, const char *key, const cJSON *value, uint32_t layer,
                                 cJSON **refusal) {
    spisor_refusal refused = SPISOR_INIT(spisor_refusal);
    cJSON *previous = NULL;
    int secret = 0;
    spisor_status status;
    request->key = key;
    request->value = value;
    request->layer = layer;
    status = runtime_config_set_value(request, &previous, &secret, &refused);
    cJSON_Delete(previous);
    *refusal = refusal_json(status, &refused);
    return status == SPISOR_OK;
}

int runtime_set_ecosystem_setting(const char *home, const char *portable, const char *key, const cJSON *value,
                                  uint32_t layer, cJSON **refusal) {
    runtime_config_request request = runtime_config_request_of(home, portable);
    return set_ecosystem_setting(&request, key, value, layer, refusal);
}

int runtime_merge_ecosystem_setting(const char *home, const char *portable, const char *key, const cJSON *value,
                                    uint32_t layer, cJSON **refusal) {
    runtime_config_request request = runtime_config_request_of(home, portable);
    request.merges = 1;
    return set_ecosystem_setting(&request, key, value, layer, refusal);
}
