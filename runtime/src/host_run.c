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
#define USAGE_MESSAGE                                                                                               \
    "the host takes a loader id, or a loader id followed by action, a plugin, an action and its input as a JSON "   \
    "object"
#define USAGE_FIX "start it with <loader id>, or with action <plugin> <action> <input json object> after the loader id"
#define SCOPES_FIX "check that the loader's registered home and the places are absolute folders, then try again"

/** What a run's arguments name: its loader, its mode and, for an ACTION run, the plugin, the action and its input. */
typedef struct host_request {
    const char *host_id;
    uint32_t mode;
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
    request->host_id = count > 0 ? arguments[0] : NULL;
    request->mode = count == 1 ? WEIVEL_STANDARD_HOST_SERVE : 0;
    if (count == 5 && strcmp(arguments[1], ACTION_WORD) == 0) {
        request->input = spisor_parse_json_reporting((const uint8_t *)arguments[4], strlen(arguments[4]),
                                                     &out_of_memory);
        request->mode = cJSON_IsObject(request->input) ? WEIVEL_STANDARD_HOST_ACTION : 0;
        request->plugin = arguments[2];
        request->action = arguments[3];
    }
    if (request->mode == 0 && !out_of_memory) {
        *refusal = runtime_refusal(USAGE_MESSAGE, USAGE_FIX);
    }
    return request->mode != 0;
}

/** The javascript-host's hint, {libraries, host}, refused unless it names the version the layer runs at. */
static cJSON *layer_hint(const char *cache, cJSON **refusal) {
    cJSON *read = runtime_boot_read(cache, RUNTIME_JAVASCRIPT_HOST_ID, refusal);
    cJSON *hint = cJSON_DetachItemFromObjectCaseSensitive(read, "hint");
    cJSON_Delete(read);
    if (read != NULL && text_of(hint, "host") == NULL) {
        const char *parts[3] = {"no " RUNTIME_JAVASCRIPT_HOST_ID " version is recorded in ", cache,
                                ", which runs the plugins the loader's settings enable"};
        *refusal = refused(parts, 3, RUNTIME_REINSTALL_FIX);
        cJSON_Delete(hint);
        hint = NULL;
    }
    return hint;
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
    runtime_hosts_start_request start = {environment,  request->host_id, RUNTIME_JAVASCRIPT_HOST_ID,
                                         text_of(hint, "host"), versions,
                                         request->mode == WEIVEL_STANDARD_HOST_SERVE};
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
    cJSON *action = cJSON_CreateObject();
    int built = action != NULL && cJSON_AddStringToObject(action, "plugin", manifest) != NULL &&
                cJSON_AddStringToObject(action, "name", name) != NULL &&
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
    return run;
}

static void free_run(runtime_host_run *run) {
    cJSON_Delete(run->started);
    cJSON_Delete(run->located);
    cJSON_Delete(run->libraries);
    cJSON_Delete(run->action);
    spisor_memory_release(run->host_id);
    spisor_memory_release(run);
}

/** Fills everything the run plans past hosts.start; 0 with `*refusal` as start says, the scopes then not begun. */
static int planned(runtime_host_run *run, const host_request *request, const cJSON *hint, cJSON **refusal) {
    const char *cache = text_of(cJSON_GetObjectItemCaseSensitive(run->started, "places"), "cache");
    const cJSON *plan = cJSON_GetObjectItemCaseSensitive(run->started, "plan");
    run->libraries = planned_libraries(hint);
    const cJSON *plugins = cJSON_GetObjectItemCaseSensitive(plan, "plugins");
    run->located = run->libraries == NULL ? NULL : runtime_cache_locate_plugins(cache, plugins);
    if (run->located != NULL && request->mode == WEIVEL_STANDARD_HOST_ACTION) {
        run->action = planned_action(request, run->located, refusal);
    }
    return run->located != NULL && (request->mode != WEIVEL_STANDARD_HOST_ACTION || run->action != NULL) &&
           scopes_began(request, run->started, refusal);
}

/**
 * Publishes a SERVE run's refusal to the loader's ready record, so a launch waiting on it reads why it will not serve.
 *
 * @implNote Best effort, as hosts.start publishes its own, which is why a refusal of hosts.start is not published
 * again: the run is refused either way.
 */
static void publish_refusal(const host_request *request, const cJSON *places, const cJSON *refusal) {
    if (request->mode == WEIVEL_STANDARD_HOST_SERVE && refusal != NULL && runtime_is_plain_host_id(request->host_id)) {
        runtime_publish_ready_refusal(text_of(places, "state"), request->host_id, refusal);
    }
}

static runtime_host_run *run_of(const host_request *request, const cJSON *environment, const cJSON *hint,
                                cJSON **refusal) {
    runtime_host_run *run = new_run(request);
    if (run != NULL) {
        run->started = started_run(request, environment, hint, refusal);
    }
    if (run != NULL && run->started != NULL && !planned(run, request, hint, refusal)) {
        publish_refusal(request, cJSON_GetObjectItemCaseSensitive(run->started, "places"), *refusal);
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
    hint = places == NULL ? NULL : layer_hint(text_of(places, "cache"), refusal);
    if (hint == NULL && places != NULL) {
        publish_refusal(&request, places, *refusal);
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
