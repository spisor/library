#include "host_run.h"
#include "allocation.h"

#include <string.h>

#include <weivel/host.h>

#include "boot.h"
#include "boot_hint.h"
#include "cache_locate.h"
#include "cache_refusal.h"
#include "host_references.h"
#include "hosts_start.h"
#include "install_entries.h"
#include "json_service.h"
#include "node_json_file.h"
#include "paths.h"
#include "places.h"
#include "processes_ready.h"
#include "registry.h"
#include "scope_host.h"
#include "spisor/json_kind.h"
#include "spisor/path.h"
#include "ui.h"

#define ACTION_WORD "action"
#define IN_PROCESS_WORD "in-process"
#define USAGE_MESSAGE                                                                                             \
    "the host takes its layer's runtime, javascript or native, then a loader id, alone, followed by in-process, " \
    "or followed by action, a plugin, an action and its input as a JSON object"
#define USAGE_FIX                                                                                                 \
    "start it with <runtime> <loader id>, with in-process after the loader id, or with action <plugin> <action> " \
    "<input json object> after the loader id"
#define SCOPES_FIX "check that the loader's registered home and the places are absolute folders, then try again"

/**
 * What a run's arguments name: its layer's runtime and the layer item that runtime names, its loader, its mode,
 * whether a SERVE run serves its own process alone, and, for an ACTION run, the plugin, the action and its input.
 */
typedef struct host_request {
    const char *runtime;
    const char *layer_id;
    const char *host_id;
    uint32_t mode;
    int in_process;
    const char *plugin;
    const char *action;
    cJSON *input;
} host_request;

static const char *text_of(const cJSON *object, const char *member) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, member));
}

static cJSON *refused(const char *const *parts, size_t count, const char *fix) {
    return runtime_cache_refusal(parts, count, fix);
}

/** 1 with `request` filled; 0 with `*refusal` set for arguments naming no run, or NULL when memory runs out. */
static int read_request(const char *const *arguments, size_t count, host_request *request, cJSON **refusal) {
    int out_of_memory = 0;
    memset(request, 0, sizeof *request);
    request->runtime = count > 0 ? arguments[0] : NULL;
    request->layer_id = runtime_layer_item_of(request->runtime);
    request->host_id = count > 1 ? arguments[1] : NULL;
    request->mode = count == 2 ? WEIVEL_STANDARD_HOST_SERVE : 0;
    if (count == 3 && strcmp(arguments[2], IN_PROCESS_WORD) == 0) {
        request->mode = WEIVEL_STANDARD_HOST_SERVE;
        request->in_process = 1;
    }
    if (count == 6 && strcmp(arguments[2], ACTION_WORD) == 0) {
        request->input = spisor_parse_json_reporting((const uint8_t *)arguments[5], strlen(arguments[5]),
                                                     &out_of_memory);
        request->mode = cJSON_IsObject(request->input) ? WEIVEL_STANDARD_HOST_ACTION : 0;
        request->plugin = arguments[3];
        request->action = arguments[4];
    }
    if (request->layer_id == NULL) {
        request->mode = 0;
    }
    if (request->mode == 0 && !out_of_memory) {
        *refusal = runtime_refusal(USAGE_MESSAGE, USAGE_FIX);
    }
    return request->mode != 0;
}

/** Each hinted library's id to its version, as hosts.start takes the libraries the layer loaded. */
static cJSON *library_versions(const cJSON *hint) {
    cJSON *versions = cJSON_CreateObject();
    const cJSON *library;
    cJSON_ArrayForEach(library, cJSON_GetObjectItemCaseSensitive(hint, "libraries")) {
        if (versions != NULL &&
            cJSON_AddStringToObject(versions, library->string, text_of(library, "version")) == NULL) {
            cJSON_Delete(versions);
            versions = NULL;
        }
    }
    return versions;
}

static cJSON *started_run(const host_request *request, const cJSON *environment, const cJSON *hint,
                          cJSON **refusal) {
    cJSON *versions = library_versions(hint);
    runtime_hosts_start_request start = {environment,
                                         request->host_id,
                                         request->layer_id,
                                         text_of(hint, "host"),
                                         versions,
                                         request->mode == WEIVEL_STANDARD_HOST_SERVE,
                                         request->in_process,
                                         request->runtime,
                                         request->mode == WEIVEL_STANDARD_HOST_ACTION ? request->plugin : NULL};
    cJSON *started = versions == NULL ? NULL : runtime_hosts_start(&start, refusal);
    cJSON_Delete(versions);
    return started;
}

static int add_library(cJSON *libraries, const cJSON *library) {
    const char *segments[2] = {text_of(library, "directory"), RUNTIME_LIBRARY_DECLARATION};
    char *declaration = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
    cJSON *planned = declaration == NULL ? NULL : cJSON_CreateObject();
    int added = planned != NULL && cJSON_AddStringToObject(planned, "binary", text_of(library, "binary")) != NULL &&
                cJSON_AddStringToObject(planned, "declaration", declaration) != NULL &&
                cJSON_AddItemToArray(libraries, planned);
    if (!added) {
        cJSON_Delete(planned);
    }
    spisor_memory_release(declaration);
    return added;
}

/** The hinted libraries the program loads: each with a binary, but runtime, which the program was started with. */
static cJSON *planned_libraries(const cJSON *hint) {
    cJSON *libraries = cJSON_CreateArray();
    const cJSON *library;
    cJSON_ArrayForEach(library, cJSON_GetObjectItemCaseSensitive(hint, "libraries")) {
        int loads = text_of(library, "binary") != NULL && strcmp(library->string, RUNTIME_LIBRARY_ID) != 0;
        if (libraries != NULL && loads && !add_library(libraries, library)) {
            cJSON_Delete(libraries);
            libraries = NULL;
        }
    }
    return libraries;
}

static const char *located_manifest(const cJSON *located, const char *plugin) {
    const cJSON *found;
    cJSON_ArrayForEach(found, cJSON_GetObjectItemCaseSensitive(located, "located")) {
        if (strcmp(text_of(found, "id"), plugin) == 0) {
            return text_of(found, "manifest");
        }
    }
    return NULL;
}

static cJSON *action_of(const char *manifest, const cJSON *call, const char *name) {
    const cJSON *binding = cJSON_GetObjectItemCaseSensitive(call, "binding");
    cJSON *action = cJSON_CreateObject();
    int built = action != NULL && cJSON_AddStringToObject(action, "plugin", manifest) != NULL &&
                cJSON_AddStringToObject(action, "name", name) != NULL &&
                cJSON_AddStringToObject(action, "service", text_of(binding, "service")) != NULL &&
                cJSON_AddStringToObject(action, "method", text_of(binding, "method")) != NULL &&
                runtime_add_owned(action, "arguments",
                                  cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(call, "arguments"), 1));
    if (!built) {
        cJSON_Delete(action);
        return NULL;
    }
    return action;
}

/** The action to run: the manifest of the plan's plugin of that id, the action, and its merged arguments. */
static cJSON *planned_action(const host_request *request, const cJSON *located, cJSON **refusal) {
    const char *manifest = located_manifest(located, request->plugin);
    int out_of_memory = 0;
    cJSON *description = manifest == NULL ? NULL : runtime_read_node_json(manifest, &out_of_memory);
    cJSON *call = description == NULL ? NULL : runtime_ui_action(description, request->action, request->input, NULL,
                                                                 refusal);
    cJSON *action = call == NULL ? NULL : action_of(manifest, call, request->action);
    if (manifest == NULL) {
        const char *parts[4] = {request->plugin, " is not among the plugins ", request->host_id, " runs"};
        *refusal = refused(parts, 4, "enable the plugin in the loader's settings and install it, then try again");
    } else if (description == NULL && !out_of_memory) {
        const char *parts[4] = {"the manifest ", manifest, " of ", request->plugin};
        *refusal = refused(parts, 4, RUNTIME_REINSTALL_FIX);
    }
    cJSON_Delete(call);
    cJSON_Delete(description);
    return action;
}

/** Begins the scopes over the loader's home and the places hosts.start answered; 0 with `*refusal` as start says. */
static int scopes_began(const host_request *request, const cJSON *started, cJSON **refusal) {
    const cJSON *places = cJSON_GetObjectItemCaseSensitive(started, "places");
    runtime_scope_host host = {text_of(cJSON_GetObjectItemCaseSensitive(started, "identity"), "home"), NULL,
                               text_of(places, "portable"), text_of(places, "cache"), text_of(places, "secretsFile"),
                               cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(places, "forceFileStore")) ? 1u : 0u,
                               cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(started, "noColor")) ? 1u : 0u};
    spisor_status status = runtime_scopes_begin(&host);
    if (status != SPISOR_OK && status != SPISOR_OUT_OF_MEMORY) {
        const char *parts[2] = {"the plugins' scopes could not begin for ", request->host_id};
        *refusal = refused(parts, 2, SCOPES_FIX);
    }
    return status == SPISOR_OK;
}

/** A copy of the identity hosts.start answered for the run; NULL when memory runs out. */
static char *run_id_of(const cJSON *started) {
    const char *run_id = text_of(started, "runId");
    char *copy = run_id == NULL ? NULL : spisor_memory_allocate(strlen(run_id) + 1);
    if (copy != NULL) {
        strcpy(copy, run_id);
    }
    return copy;
}

static runtime_host_run *new_run(const host_request *request) {
    runtime_host_run *run = spisor_memory_allocate(sizeof *run);
    char *host_id = run == NULL ? NULL : spisor_memory_allocate(strlen(request->host_id) + 1);
    if (host_id == NULL) {
        spisor_memory_release(run);
        return NULL;
    }
    memset(run, 0, sizeof *run);
    strcpy(host_id, request->host_id);
    run->host_id = host_id;
    run->mode = request->mode;
    run->in_process = request->in_process ? 1 : 0;
    run->exit_code = request->mode == WEIVEL_STANDARD_HOST_ACTION ? RUNTIME_HOST_EXIT_FAILED : 0;
    return run;
}

static void free_run(runtime_host_run *run) {
    cJSON_Delete(run->started);
    cJSON_Delete(run->located);
    cJSON_Delete(run->libraries);
    cJSON_Delete(run->action);
    spisor_memory_release(run->run_id);
    spisor_memory_release(run->host_id);
    spisor_memory_release(run);
}

/**
 * 1 when every plugin located has an entry; 0 with `*refusal` set naming the first that has none, since the host
 * program activates a plugin only through its entry and a plan has no plugin without one.
 */
static int has_every_entry(const cJSON *located, cJSON **refusal) {
    const cJSON *found;
    cJSON_ArrayForEach(found, cJSON_GetObjectItemCaseSensitive(located, "located")) {
        if (text_of(found, "entry") == NULL) {
            const char *parts[5] = {text_of(found, "id"), "@", text_of(found, "version"), "'s manifest ",
                                    text_of(found, "manifest")};
            char *start = runtime_cache_text(parts, 5);
            const char *message[2] = {start, " names no entry installed beside it, so the host cannot activate it"};
            *refusal = start == NULL ? NULL : refused(message, 2, RUNTIME_REINSTALL_FIX);
            spisor_memory_release(start);
            return 0;
        }
    }
    return 1;
}

/** Fills everything the run plans past hosts.start; 0 with `*refusal` as start says, the scopes then not begun. */
static int planned(runtime_host_run *run, const host_request *request, const cJSON *hint, cJSON **refusal) {
    const char *cache = text_of(cJSON_GetObjectItemCaseSensitive(run->started, "places"), "cache");
    const cJSON *plan = cJSON_GetObjectItemCaseSensitive(run->started, "plan");
    run->libraries = planned_libraries(hint);
    const cJSON *plugins = cJSON_GetObjectItemCaseSensitive(plan, "plugins");
    run->located = run->libraries == NULL ? NULL : runtime_cache_locate_plugins(cache, plugins);
    int located = run->located != NULL && has_every_entry(run->located, refusal);
    if (located && request->mode == WEIVEL_STANDARD_HOST_ACTION) {
        run->action = planned_action(request, run->located, refusal);
    }
    return located && (request->mode != WEIVEL_STANDARD_HOST_ACTION || run->action != NULL) &&
           scopes_began(request, run->started, refusal);
}

/**
 * Publishes a SERVE run's refusal to the ready record of run_id, the run's identity, else of the loader before its
 * identity is known, so a launch waiting on it reads why it will not serve.
 *
 * @implNote Best effort, as hosts.start publishes its own, which is why a refusal of hosts.start is not published
 * again: the run is refused either way.
 */
static void publish_refusal(const host_request *request, const char *run_id, const cJSON *places,
                            const cJSON *refusal) {
    if (request->mode == WEIVEL_STANDARD_HOST_SERVE && !request->in_process && refusal != NULL &&
        runtime_is_plain_host_id(request->host_id)) {
        runtime_publish_ready_refusal(text_of(places, "state"), run_id == NULL ? request->host_id : run_id, refusal);
    }
}

/**
 * Publishes the refusal of a SERVE run whose layer has no hint under the identity its layer runs as, so a launch
 * waiting on that layer reads it, else under the loader's when the identity cannot be read.
 *
 * @implNote The refusal is the layer's own: publishing it under the loader's identity while another layer holds the
 * front door would overwrite that layer's ready record and leave this layer's waited on until it times out.
 */
static void publish_unhinted_refusal(const host_request *request, const cJSON *places, const cJSON *refusal) {
    runtime_loader_layers layers = {NULL, NULL, NULL, NULL, NULL, NULL, NULL};
    cJSON *unread = NULL;
    int read = refusal != NULL && request->mode == WEIVEL_STANDARD_HOST_SERVE && !request->in_process &&
               runtime_is_plain_host_id(request->host_id) &&
               runtime_read_loader_layers(places, request->host_id, &layers, &unread);
    char *identity = read ? runtime_run_identity(request->host_id, request->runtime, layers.front_door) : NULL;
    publish_refusal(request, identity, places, refusal);
    spisor_memory_release(identity);
    cJSON_Delete(unread);
    if (read) {
        runtime_free_loader_layers(&layers);
    }
}

static runtime_host_run *run_of(const host_request *request, const cJSON *environment, const cJSON *hint,
                                cJSON **refusal) {
    runtime_host_run *run = new_run(request);
    if (run != NULL) {
        run->started = started_run(request, environment, hint, refusal);
        run->run_id = run->started == NULL ? NULL : run_id_of(run->started);
    }
    if (run != NULL && run->started != NULL && (run->run_id == NULL || !planned(run, request, hint, refusal))) {
        publish_refusal(request, run->run_id, cJSON_GetObjectItemCaseSensitive(run->started, "places"), *refusal);
        runtime_hosts_start_undo(cJSON_GetObjectItemCaseSensitive(run->started, "plan"));
        free_run(run);
        run = NULL;
    } else if (run != NULL && run->started == NULL) {
        free_run(run);
        run = NULL;
    }
    return run;
}

runtime_host_run *runtime_host_run_start(const char *const *arguments, size_t argument_count,
                                         const cJSON *environment, cJSON **refusal) {
    host_request request;
    cJSON *places = NULL;
    cJSON *hint = NULL;
    runtime_host_run *run = NULL;
    *refusal = NULL;
    if (read_request(arguments, argument_count, &request, refusal)) {
        places = runtime_places_of_environment(environment, refusal);
    }
    hint = places == NULL ? NULL : runtime_layer_hint(text_of(places, "cache"), request.layer_id, refusal);
    if (hint == NULL && places != NULL) {
        publish_unhinted_refusal(&request, places, *refusal);
    }
    run = hint == NULL ? NULL : run_of(&request, environment, hint, refusal);
    cJSON_Delete(request.input);
    cJSON_Delete(hint);
    cJSON_Delete(places);
    return run;
}

void runtime_host_run_end(runtime_host_run *run) {
    if (run == NULL) {
        return;
    }
    runtime_scopes_end();
    runtime_hosts_start_undo(cJSON_GetObjectItemCaseSensitive(run->started, "plan"));
    free_run(run);
}
