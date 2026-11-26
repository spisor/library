#include "hosts_layer_start.h"
#include "allocation.h"

#include <string.h>

#include "cache_marker.h"
#include "cache_place.h"
#include "cache_refusal.h"
#include "hosts_layers.h"
#include "install_entries.h"
#include "install_runtime.h"
#include "json_service.h"
#include "node_json_file.h"
#include "paths.h"
#include "places.h"
#include "registry.h"
#include "runnable.h"
#include "runtime/hosts.h"
#include "spisor/path.h"

#define HOST_WORD "host"
#define IN_PROCESS_WORD "in-process"
#define ACTION_WORD "action"

static const char *text_of(const cJSON *object, const char *member) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, member));
}

static cJSON *reinstall_refusal(const char *first, const char *second, const char *third) {
    const char *parts[3] = {first, second, third};
    return runtime_cache_refusal(parts, 3, RUNTIME_REINSTALL_FIX);
}

/** cache/bundles/<id>/<version>, refused unless that version is installed with every file it lists at its recorded size. */
static char *installed_bundle(const runtime_entry_start_request *request, cJSON **refusal) {
    const char *segments[4] = {request->cache, "bundles", request->id, request->version};
    char *directory = request->version == NULL ? NULL : spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 4);
    cJSON *check = directory == NULL ? NULL : runtime_cache_check_version_sizes(directory);
    const char *kind = text_of(check, "kind");
    int intact = kind != NULL && strcmp(kind, "intact") == 0;
    if (request->version == NULL) {
        *refusal = reinstall_refusal("no installed version of ", request->id, " is recorded");
    } else if (check != NULL && !intact) {
        *refusal = reinstall_refusal(request->id, " is not installed intact in ", directory);
    }
    if (!intact) {
        spisor_memory_release(directory);
        directory = NULL;
    }
    cJSON_Delete(check);
    return directory;
}

/** The entry the bundle's host.json names, NULL when it names none, and NULL with *out_of_memory set when memory runs out. */
static char *entry_of_bundle(const char *directory, int *out_of_memory) {
    const char *segments[2] = {directory, "host.json"};
    char *path = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
    cJSON *standard = path == NULL ? NULL : runtime_read_node_json(path, out_of_memory);
    const char *entry = text_of(standard, "entry");
    char *copy = entry == NULL ? NULL : runtime_cache_text(&entry, 1);
    *out_of_memory = *out_of_memory || path == NULL || (entry != NULL && copy == NULL);
    cJSON_Delete(standard);
    spisor_memory_release(path);
    return copy;
}

/** The file entry names inside directory, refused when it names none or one outside it. */
static char *entry_inside(const char *directory, const char *id, const char *entry, cJSON **refusal) {
    char *path = entry == NULL ? NULL : runtime_cache_resolve(directory, &entry, 1);
    int out_of_memory = entry != NULL && path == NULL;
    if (path != NULL && !runtime_cache_is_strictly_inside(directory, path)) {
        spisor_memory_release(path);
        path = NULL;
    }
    if (path == NULL && !out_of_memory) {
        *refusal = reinstall_refusal("the start recorded for ", id, " names no entry inside its bundle");
    }
    return path;
}

/** The entry's path inside its installed bundle, refused as runtime_entry_start says. */
static char *entry_path(const runtime_entry_start_request *request, cJSON **refusal) {
    char *directory = installed_bundle(request, refusal);
    int out_of_memory = 0;
    char *named = directory == NULL || request->entry != NULL ? NULL : entry_of_bundle(directory, &out_of_memory);
    const char *entry = request->entry != NULL ? request->entry : named;
    char *path = directory == NULL || out_of_memory ? NULL : entry_inside(directory, request->id, entry, refusal);
    spisor_memory_release(named);
    spisor_memory_release(directory);
    return path;
}

static int add_text(cJSON *array, const char *text) {
    cJSON *item = cJSON_CreateString(text);
    if (item == NULL || !cJSON_AddItemToArray(array, item)) {
        cJSON_Delete(item);
        return 0;
    }
    return 1;
}

/** {executable, arguments: [script?, ...arguments], environment}; NULL when memory runs out. */
static cJSON *start_of(const runtime_entry_start_request *request, const char *executable, const char *script) {
    cJSON *start = cJSON_CreateObject();
    cJSON *arguments = start == NULL ? NULL : cJSON_AddArrayToObject(start, "arguments");
    int built = arguments != NULL && cJSON_AddStringToObject(start, "executable", executable) != NULL &&
                (script == NULL || add_text(arguments, script));
    const cJSON *argument;
    cJSON_ArrayForEach(argument, request->arguments) {
        built = built && add_text(arguments, argument->valuestring);
    }
    built = built && runtime_add_owned(start, "environment", cJSON_Duplicate(request->environment, 1));
    if (!built) {
        cJSON_Delete(start);
        return NULL;
    }
    return start;
}

cJSON *runtime_entry_start(const runtime_entry_start_request *request, cJSON **refusal) {
    int native = request->runtime != NULL && strcmp(request->runtime, RUNTIME_LAYER_NATIVE) == 0;
    int javascript = request->runtime != NULL && strcmp(request->runtime, RUNTIME_LAYER_JAVASCRIPT) == 0;
    char *path = NULL;
    char *executable = NULL;
    cJSON *start = NULL;
    *refusal = NULL;
    if (!native && !javascript) {
        *refusal = reinstall_refusal("the start recorded for ", request->id, " names a runtime this host cannot start");
        return NULL;
    }
    path = entry_path(request, refusal);
    if (path != NULL && native && !runtime_make_runnable(path)) {
        *refusal = reinstall_refusal("the entry of ", request->id, " cannot be made runnable");
    } else if (path != NULL && native) {
        start = start_of(request, path, NULL);
    } else if (path != NULL) {
        executable = runtime_javascript_executable(request->cache, request->environment, refusal);
        start = executable == NULL ? NULL : start_of(request, executable, path);
    }
    spisor_memory_release(executable);
    spisor_memory_release(path);
    return start;
}

/** What a layer start reads once for every layer it answers. */
typedef struct layer_context {
    const runtime_hosts_layer_start_request *request;
    const char *cache;
    runtime_loader_layers layers;
} layer_context;

/** The arguments a layer of runtime is given: its leading words, the loader id, then those of the mode. */
static cJSON *layer_arguments(const layer_context *context, const char *runtime, const char *binary) {
    const runtime_hosts_layer_start_request *request = context->request;
    cJSON *arguments = cJSON_CreateArray();
    int native = strcmp(runtime, RUNTIME_LAYER_NATIVE) == 0;
    int built = arguments != NULL && (!native || add_text(arguments, HOST_WORD)) && add_text(arguments, binary) &&
                add_text(arguments, runtime) && add_text(arguments, request->loader_id);
    if (request->mode == RUNTIME_HOSTS_LAYER_IN_PROCESS) {
        built = built && add_text(arguments, IN_PROCESS_WORD);
    } else if (request->mode == RUNTIME_HOSTS_LAYER_ACTION) {
        built = built && add_text(arguments, ACTION_WORD) && add_text(arguments, request->plugin) &&
                add_text(arguments, request->action) && add_text(arguments, request->input);
    }
    if (!built) {
        cJSON_Delete(arguments);
        return NULL;
    }
    return arguments;
}

/** The runtime binary the layer's hint names, refused with the reinstall fix when it names none. */
static const char *runtime_binary(const cJSON *hint, const char *layer_id, cJSON **refusal) {
    const cJSON *runtime = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(hint, "libraries"),
                                                            RUNTIME_LIBRARY_ID);
    const char *binary = text_of(runtime, "binary");
    if (binary == NULL) {
        *refusal = reinstall_refusal("the hint of ", layer_id, " names no runtime binary to load");
    }
    return binary;
}

/** {runtime, hostId}, the start of the layer of runtime added to it; NULL with *refusal as hosts.layer_start says. */
static cJSON *layer_of(const layer_context *context, const char *runtime, cJSON **refusal) {
    const char *layer_id = runtime_layer_item_of(runtime);
    cJSON *hint = runtime_layer_hint(context->cache, layer_id, refusal);
    const char *binary = hint == NULL ? NULL : runtime_binary(hint, layer_id, refusal);
    cJSON *arguments = binary == NULL ? NULL : layer_arguments(context, runtime, binary);
    runtime_entry_start_request entry = {context->cache, context->request->environment, layer_id,
                                         text_of(hint, "host"), runtime, NULL, arguments};
    cJSON *layer = arguments == NULL ? NULL : runtime_entry_start(&entry, refusal);
    char *identity = layer == NULL ? NULL
                                   : runtime_run_identity(context->request->loader_id, runtime,
                                                          context->layers.front_door);
    int built = identity != NULL && cJSON_AddStringToObject(layer, "runtime", runtime) != NULL &&
                cJSON_AddStringToObject(layer, "hostId", identity) != NULL;
    if (!built) {
        cJSON_Delete(layer);
        layer = NULL;
    }
    spisor_memory_release(identity);
    cJSON_Delete(arguments);
    cJSON_Delete(hint);
    return layer;
}

static int add_layer(const layer_context *context, cJSON *layers, const char *runtime, cJSON **refusal) {
    cJSON *layer = layer_of(context, runtime, refusal);
    if (layer == NULL || !cJSON_AddItemToArray(layers, layer)) {
        cJSON_Delete(layer);
        return 0;
    }
    return 1;
}

/** Whether a SERVE or IN_PROCESS start runs a layer of runtime: one of its plugins does, or, for the front door, the standard requires one. */
static int serves_on(const layer_context *context, const char *runtime) {
    const runtime_loader_layers *layers = &context->layers;
    int requires = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(layers->standard, "plugins")) > 0;
    return runtime_names_plugin_on(layers->catalog, layers->plugins, runtime) ||
           (requires && strcmp(runtime, layers->front_door) == 0);
}

/** Every layer a SERVE or IN_PROCESS start runs, the front door's last. */
static int add_serving_layers(const layer_context *context, cJSON *layers, cJSON **refusal) {
    static const char *const runtimes[2] = {RUNTIME_LAYER_JAVASCRIPT, RUNTIME_LAYER_NATIVE};
    const char *door = context->layers.front_door;
    int added = 1;
    for (size_t index = 0; added && index < 2; index++) {
        if (strcmp(runtimes[index], door) != 0 && serves_on(context, runtimes[index])) {
            added = add_layer(context, layers, runtimes[index], refusal);
        }
    }
    return added && (!runtime_is_layer_runtime(door) || !serves_on(context, door) ||
                     add_layer(context, layers, door, refusal));
}

/** The one layer an ACTION runs in, the plugin's, refused when its catalog runtime is no layer's. */
static int add_action_layer(const layer_context *context, cJSON *layers, cJSON **refusal) {
    const char *plugin = context->request->plugin;
    const char *runtime = runtime_plugin_runtime(context->layers.catalog, plugin);
    if (!runtime_is_layer_runtime(runtime)) {
        const char *parts[3] = {plugin, " runs on ", runtime};
        const char *fix_parts[3] = {"install a version of ", plugin, " written for javascript or native"};
        char *fix = runtime_cache_text(fix_parts, 3);
        *refusal = fix == NULL ? NULL : runtime_cache_refusal(parts, 3, fix);
        spisor_memory_release(fix);
        return 0;
    }
    return add_layer(context, layers, runtime, refusal);
}

cJSON *runtime_hosts_layer_start(const runtime_hosts_layer_start_request *request, cJSON **refusal) {
    layer_context context;
    cJSON *places = runtime_places_of_environment(request->environment, refusal);
    cJSON *answer = NULL;
    memset(&context, 0, sizeof context);
    context.request = request;
    context.cache = text_of(places, "cache");
    if (places != NULL && runtime_read_loader_layers(places, request->loader_id, &context.layers, refusal)) {
        cJSON *layers = cJSON_CreateArray();
        int added = layers != NULL && (request->mode == RUNTIME_HOSTS_LAYER_ACTION
                                           ? add_action_layer(&context, layers, refusal)
                                           : add_serving_layers(&context, layers, refusal));
        answer = added ? cJSON_CreateObject() : NULL;
        if (answer == NULL || !cJSON_AddItemToObject(answer, "layers", layers)) {
            cJSON_Delete(answer);
            cJSON_Delete(layers);
            answer = NULL;
        }
        runtime_free_loader_layers(&context.layers);
    }
    cJSON_Delete(places);
    return answer;
}
