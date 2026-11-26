#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_child.h"
#include "allocation.h"
#include "host_commands.h"
#include "host_context.h"
#include "host_holds.h"
#include "host_output.h"
#include "host_start.h"
#include "loader_start.h"
#include "on_path.h"
#include "own_run.h"
#include "paths.h"
#include "required_plugins.h"
#include "runtime/hosts.h"
#include "session_ready.h"

#define HOLD_FIX "nothing is needed now; the application runs, and a background host it lacks starts with the next launch"

/** What a launch holds: each host with its start, and the context the application's own hold is taken in. */
typedef struct launch_holds {
    const host_context *context;
    cJSON *starts;
    double application;
    own_run run;
} launch_holds;

/** The application's binary on the PATH, refused naming how to put it there. */
static char *application_binary(const host_context *context, const cJSON *standard, cJSON **refusal) {
    const cJSON *application = cJSON_GetObjectItemCaseSensitive(standard, "application");
    const char *binary = host_text(application, "binary");
    const char *label = host_text(application, "label");
    char *found = NULL;
    runtime_on_path_status searched = binary == NULL ? RUNTIME_ON_PATH_ABSENT
                                                     : runtime_find_on_path(RUNTIME_HOST_IS_WINDOWS, binary,
                                                                            context->environment, &found);
    if (searched == RUNTIME_ON_PATH_ABSENT && binary != NULL) {
        label = label == NULL ? binary : label;
        const char *parts[5] = {"the ", label, " binary ", binary, " is not on the PATH"};
        size_t length = strlen(label) * 2 + strlen(binary) * 2 + 128;
        char *fix = spisor_memory_allocate(length);
        if (fix != NULL) {
            snprintf(fix, length, "install %s so %s is on the PATH, or run spisor-host install --catalog <path or url> to install it",
                     label, binary);
        }
        *refusal = fix == NULL ? NULL : host_refusal_of(parts, 5, fix);
        spisor_memory_release(fix);
    }
    return found;
}

static cJSON *serving_layers(const host_context *context, const char *loader_id, cJSON **refusal) {
    return host_layer_starts(context, loader_id, RUNTIME_HOSTS_LAYER_SERVE, NULL, NULL, NULL, refusal);
}

/**
 * The starts of the loader's layers, or an empty array after writing to stderr why they cannot be started; NULL when
 * memory runs out.
 *
 * @implNote Never refuses the launch (RULES section 2): the application runs without its plugins rather
 * than not at all, and the next launch tries again.
 */
static cJSON *layers_or_report(const host_context *context, const char *loader_id) {
    cJSON *refusal = NULL;
    cJSON *layers = serving_layers(context, loader_id, &refusal);
    if (layers == NULL && refusal != NULL) {
        host_write(1, "spisor could not start the session host of ");
        host_write(1, loader_id);
        host_write(1, ", so the application runs without its plugins: ");
        host_print_refusal(refusal);
        layers = cJSON_CreateArray();
    }
    cJSON_Delete(refusal);
    return layers;
}

/** The starts of the layers of a loader that requires plugins, which cannot run without them; NULL with *refusal set. */
static cJSON *layers_or_refuse(const host_context *context, const char *loader_id, cJSON **refusal) {
    cJSON *layers = serving_layers(context, loader_id, refusal);
    if (layers == NULL && *refusal != NULL) {
        const char *message = host_text(*refusal, "message");
        const char *fix = host_text(*refusal, "fix");
        const char *parts[4] = {"spisor could not start the session host of ", loader_id,
                                ", which runs the plugins it requires: ", message == NULL ? "" : message};
        cJSON *explained = host_refusal_of(parts, 4, fix == NULL ? HOST_REINSTALL_FIX : fix);
        cJSON_Delete(*refusal);
        *refusal = explained;
    }
    return layers;
}

static int requires_plugins(const loader_start *loader) {
    return cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(loader->standard, "plugins")) > 0;
}

/** The layers the loader's plugins run in, which a loader with required plugins cannot do without; NULL with *refusal set. */
static cJSON *layers_of(const host_context *context, const char *loader_id, const loader_start *loader,
                        cJSON **refusal) {
    if (requires_plugins(loader)) {
        return layers_or_refuse(context, loader_id, refusal);
    }
    return cJSON_GetArraySize(loader->plugins) > 0 ? layers_or_report(context, loader_id) : cJSON_CreateArray();
}

/** Adds each layer's start to starts under the identity hosts' layer_start answered for it; 0 when memory runs out. */
static int add_layer_starts(cJSON *starts, const cJSON *layers) {
    const cJSON *layer;
    cJSON_ArrayForEach(layer, layers) {
        cJSON *start = cJSON_Duplicate(layer, 1);
        cJSON_DeleteItemFromObjectCaseSensitive(start, "runtime");
        cJSON_DeleteItemFromObjectCaseSensitive(start, "hostId");
        if (start == NULL || !cJSON_AddItemToObject(starts, host_text(layer, "hostId"), start)) {
            cJSON_Delete(start);
            return 0;
        }
    }
    return 1;
}

/**
 * {host id: start} for every background host the loader's host.json names, then for each layer its plugins run in,
 * under that layer's identity; NULL with *refusal set when a background host or a layer of a loader with required
 * plugins cannot be started, before anything is. *layers is the layers, in the order hosts' layer_start answered them,
 * the front door's first, and starts holds them in that order after the background hosts.
 */
static cJSON *starts_of(const host_context *context, const char *loader_id, const loader_start *loader, cJSON **layers,
                        cJSON **refusal) {
    cJSON *starts = cJSON_CreateObject();
    const cJSON *background = cJSON_GetObjectItemCaseSensitive(loader->standard, "background");
    *layers = NULL;
    for (const cJSON *id = starts == NULL || background == NULL ? NULL : background->child; id != NULL; id = id->next) {
        cJSON *start = cJSON_IsString(id) ? host_start_of(context, id->valuestring, NULL, 0, refusal) : NULL;
        if (start == NULL || !cJSON_AddItemToObject(starts, id->valuestring, start)) {
            cJSON_Delete(start);
            cJSON_Delete(starts);
            return NULL;
        }
    }
    *layers = starts == NULL ? NULL : layers_of(context, loader_id, loader, refusal);
    if (*layers == NULL || !add_layer_starts(starts, *layers)) {
        cJSON_Delete(*layers);
        cJSON_Delete(starts);
        *layers = NULL;
        return NULL;
    }
    return starts;
}

/**
 * Holds every host for the application's pid as well, so a launcher killed hard leaves each one serving
 * the application until it exits.
 *
 * @implNote The application already runs, so a hold that cannot be taken is written to stderr with its fix
 * and never ends the launch.
 */
static void hold_for_application(double pid, void *context) {
    launch_holds *holds = context;
    cJSON *refusal = NULL;
    if (host_hold_all(holds->context, holds->starts, pid, NULL, &refusal)) {
        holds->application = pid;
        return;
    }
    host_write(1, "spisor could not hold the background hosts for the application: ");
    host_write(1, refusal == NULL ? "out of memory" : host_text(refusal, "message"));
    host_write(1, "\nfix: " HOLD_FIX "\n");
    cJSON_Delete(refusal);
}

static void stamp_own_run(void *context) {
    launch_holds *holds = context;
    host_stamp_own_run(&holds->run);
}

static cJSON *application_arguments(int count, char **arguments) {
    cJSON *list = cJSON_CreateArray();
    for (int index = 0; list != NULL && index < count; index++) {
        if (!cJSON_AddItemToArray(list, cJSON_CreateString(arguments[index]))) {
            cJSON_Delete(list);
            return NULL;
        }
    }
    return list;
}

/** What layer, held as pids records it, publishes once ready; NULL with *refusal set as the await refuses. */
static cJSON *published_by(const host_context *context, const cJSON *layer, const cJSON *pids, cJSON **refusal) {
    const char *identity = host_text(layer, "hostId");
    const cJSON *pid = cJSON_GetObjectItemCaseSensitive(pids, identity);
    return cJSON_IsNumber(pid) ? host_await_session_environment(context, identity, pid->valuedouble, refusal)
                               : cJSON_CreateObject();
}

/** An object referring to the starts from index first up to, not including, end; NULL when memory runs out. */
static cJSON *starts_between(const cJSON *starts, int first, int end) {
    cJSON *between = cJSON_CreateObject();
    for (int index = first; between != NULL && index < end; index++) {
        cJSON *start = cJSON_GetArrayItem(starts, index);
        if (!cJSON_AddItemReferenceToObject(between, start->string, start)) {
            cJSON_Delete(between);
            between = NULL;
        }
    }
    return between;
}

/** Holds the starts from first up to end for holder, their pids added to pids; 0 with *refusal set as the hold says. */
static int hold_between(const host_context *context, const cJSON *starts, int first, int end, double holder,
                        cJSON *pids, cJSON **refusal) {
    cJSON *between = starts_between(starts, first, end);
    int held = between != NULL && host_hold_all(context, between, holder, pids, refusal);
    cJSON_Delete(between);
    return held;
}

/**
 * Holds every start for holder, the background hosts and the front door's layer first, then each other layer once the
 * one before it is ready, and answers the environment the front door publishes; NULL with *refusal set when a hold is
 * refused, a layer publishes a refusal or is not ready in time, nothing then held.
 *
 * @implNote The other layer starts only once the front door's is ready: only the front door's session start applies
 * updates, and the other reuses the run it records, which exists once it is ready.
 */
static cJSON *held_and_ready(const host_context *context, const cJSON *starts, const cJSON *layers, double holder,
                             cJSON **refusal) {
    int layer_count = cJSON_GetArraySize(layers);
    int front_end = cJSON_GetArraySize(starts) - (layer_count > 1 ? layer_count - 1 : 0);
    cJSON *pids = cJSON_CreateObject();
    int held = pids != NULL && hold_between(context, starts, 0, front_end, holder, pids, refusal);
    cJSON *published = !held ? NULL
                       : layer_count == 0 ? cJSON_CreateObject()
                                          : published_by(context, cJSON_GetArrayItem(layers, 0), pids, refusal);
    for (int index = 1; published != NULL && index < layer_count; index++) {
        cJSON *other = hold_between(context, starts, front_end + index - 1, front_end + index, holder, pids, refusal)
                           ? published_by(context, cJSON_GetArrayItem(layers, index), pids, refusal)
                           : NULL;
        if (other == NULL) {
            cJSON_Delete(published);
            published = NULL;
        }
        cJSON_Delete(other);
    }
    if (held && published == NULL) {
        host_release_all(context, starts, holder);
    }
    cJSON_Delete(pids);
    return published;
}

static int run_application(const host_context *context, const char *binary, cJSON *starts, const cJSON *arguments,
                           const cJSON *environment) {
    cJSON *refusal = NULL;
    double self = host_process_id();
    launch_holds holds = {context, starts, 0, {NULL, NULL, NULL}};
    host_start_own_run(&holds.run, context);
    host_child_watch watch = {hold_for_application, holds.run.record == NULL ? NULL : stamp_own_run,
                              HOST_OWN_RUN_INTERVAL_MILLISECONDS, &holds};
    int exit_code = 1;
    int ran = host_run_child(binary, arguments, environment, &watch, &exit_code, &refusal);
    host_finish_own_run(&holds.run);
    if (holds.application != 0) {
        host_release_all(context, starts, holds.application);
    }
    host_release_all(context, starts, self);
    return ran ? exit_code : host_refuse(refusal);
}

static int run_held(const host_context *context, const cJSON *layers, const char *binary, cJSON *starts,
                    const cJSON *arguments) {
    cJSON *refusal = NULL;
    double self = host_process_id();
    cJSON *published = held_and_ready(context, starts, layers, self, &refusal);
    cJSON *environment = published == NULL ? NULL : host_overlay_environment(context->environment, published);
    cJSON_Delete(published);
    if (published != NULL && environment == NULL) {
        host_release_all(context, starts, self);
    }
    if (environment == NULL) {
        return host_refuse(refusal);
    }
    int code = run_application(context, binary, starts, arguments, environment);
    cJSON_Delete(environment);
    return code;
}

/**
 * Starts loader_id as every start does, holds the hosts it keeps running, runs the application as this
 * process's child with arguments, and answers the application's exit code once the holds are dropped.
 * Whatever can be refused (no registration, no application on the PATH, a background host that cannot be
 * started) is refused before anything is held or started.
 */
static int launch(const host_context *context, const char *loader_id, int count, char **arguments) {
    loader_start loader;
    cJSON *refusal = NULL;
    char *binary = host_start_loader(context, loader_id, &loader, &refusal) ? application_binary(context, loader.standard, &refusal)
                                                                             : NULL;
    int required = binary != NULL && host_check_required_plugins(context, loader_id, host_text(loader.registration, "home"),
                                                                 loader.standard, &refusal);
    cJSON *layers = NULL;
    cJSON *starts = required ? starts_of(context, loader_id, &loader, &layers, &refusal) : NULL;
    cJSON *passed = starts == NULL ? NULL : application_arguments(count, arguments);
    int code = passed == NULL ? host_refuse(refusal) : run_held(context, layers, binary, starts, passed);
    cJSON_Delete(passed);
    cJSON_Delete(starts);
    cJSON_Delete(layers);
    spisor_memory_release(binary);
    host_finish_loader(&loader);
    return code;
}

/** @implNote Read by hand, never as options, since every argument after the loader id is the application's, flags and a leading -- included. */
int host_command_launch(int count, char **arguments) {
    if (count < 1 || arguments[0][0] == 0) {
        return host_misuse("launch needs the loader id");
    }
    host_context context;
    cJSON *refusal = NULL;
    int code = host_open_context(&context, &refusal) ? launch(&context, arguments[0], count - 1, arguments + 1)
                                                    : host_refuse(refusal);
    host_close_context(&context);
    return code;
}
