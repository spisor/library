#include "runtime/paths.h"
#include "allocation.h"

#include <stdlib.h>
#include <string.h>

#include "definition_support.h"
#include "spisor/json_value.h"
#include "paths.h"
#include "segment.h"

#define ARGUMENT_SHAPE RUNTIME_SHAPE_THROUGH(runtime_paths_resolve_argument, places)
#define ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_paths_answer, storage)

typedef struct plugin_folders {
    char *portable;
    char *cache;
} plugin_folders;

static spisor_status refuse_places(spisor_refusal *refusal) {
    return runtime_refuse_as(refusal, SPISOR_MALFORMED,
                             "the argument carries places without usable portable and cache folders and a plugin id",
                             "send places as places' resolve answers them, with absolute portable and cache, and plugin "
                             "as the manifest id, one portable path segment such as \"my-plugin\"");
}

/** The names as the JSON the path rules read, NULL for none. */
static spisor_status names_json(const spisor_value *names, cJSON **json, spisor_refusal *refusal) {
    spisor_status status;
    *json = NULL;
    if (names == NULL) {
        return SPISOR_OK;
    }
    status = names->kind == SPISOR_VALUE_RECORD ? spisor_json_of_value(names, json) : SPISOR_MALFORMED;
    if (status == SPISOR_MALFORMED) {
        return runtime_refuse_as(refusal, status, "the names are not a well-formed record",
                                 "send names as the plugin's names declaration, a record of text members, or NULL");
    }
    return status;
}

static void free_plugin_folders(plugin_folders *folders) {
    spisor_memory_release(folders->portable);
    spisor_memory_release(folders->cache);
    folders->portable = NULL;
    folders->cache = NULL;
}

/** The plugin's folders in the argument's places, both NULL without places; 0 when the places are unusable. */
static int read_plugin_folders(const runtime_paths_resolve_argument *argument, plugin_folders *folders,
                               int *out_of_memory) {
    const runtime_plugin_places *places = argument->places;
    char *folder_name;
    *out_of_memory = 0;
    if (places == NULL) {
        return 1;
    }
    if (!runtime_has_shape(places->size, RUNTIME_SHAPE_THROUGH(runtime_plugin_places, cache))) {
        return 0;
    }
    if (argument->plugin == NULL || !runtime_trims_to_portable_segment(argument->plugin) ||
        !runtime_is_usable_home(places->portable) || !runtime_is_usable_home(places->cache)) {
        return 0;
    }
    if (runtime_portable_segment(argument->plugin, &folder_name) != SPISOR_OK) {
        *out_of_memory = 1;
        return 1;
    }
    folders->portable = runtime_plugin_folder(places->portable, folder_name);
    folders->cache = runtime_plugin_folder(places->cache, folder_name);
    spisor_memory_release(folder_name);
    *out_of_memory = folders->portable == NULL || folders->cache == NULL;
    return 1;
}

static spisor_status fill_answer(const runtime_paths *paths, const plugin_folders *folders,
                                 runtime_paths_answer *answer) {
    const char *texts[7] = {paths->home,  paths->repos,       paths->plugin, paths->cache,
                            paths->config, folders->portable, folders->cache};
    const char *packed[7];
    if (!runtime_pack_texts(texts, packed, 7, &answer->storage)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    answer->home = packed[0];
    answer->repos = packed[1];
    answer->plugin = packed[2];
    answer->cache = packed[3];
    answer->config = packed[4];
    answer->portable = packed[5];
    answer->plugin_cache = packed[6];
    return SPISOR_OK;
}

static spisor_status resolve_folders(const runtime_paths_resolve_argument *argument, const cJSON *names,
                                     runtime_paths_answer *answer, spisor_refusal *refusal) {
    plugin_folders folders = {NULL, NULL};
    runtime_paths paths;
    int out_of_memory;
    spisor_status status;
    if (!read_plugin_folders(argument, &folders, &out_of_memory)) {
        return refuse_places(refusal);
    }
    if (out_of_memory || runtime_resolve_paths(argument->home, names, &paths) != 0) {
        free_plugin_folders(&folders);
        return SPISOR_OUT_OF_MEMORY;
    }
    status = fill_answer(&paths, &folders, answer);
    runtime_free_paths(&paths);
    free_plugin_folders(&folders);
    return status;
}

static spisor_status resolve(void *context, const runtime_paths_resolve_argument *argument,
                             runtime_paths_answer *answer, spisor_refusal *refusal) {
    cJSON *names = NULL;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, ARGUMENT_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "paths' resolve");
    }
    if (!runtime_is_usable_home(argument->home)) {
        return runtime_refuse_home(refusal);
    }
    status = names_json(argument->names, &names, refusal);
    if (status == SPISOR_OK) {
        status = resolve_folders(argument, names, answer, refusal);
    }
    cJSON_Delete(names);
    return status;
}

static void release_answer(void *context, runtime_paths_answer *answer) {
    (void)context;
    runtime_release_packed_answer(answer, ANSWER_SHAPE);
}

static const runtime_paths_definition definition = {
    sizeof(runtime_paths_definition), NULL, resolve, release_answer, runtime_release_refusal,
};

RUNTIME_DEFINITION_API const void *runtime_paths_definition_of(void) {
    return &definition;
}
