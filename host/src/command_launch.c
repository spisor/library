#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_child.h"
#include "host_commands.h"
#include "host_context.h"
#include "host_holds.h"
#include "host_output.h"
#include "host_start.h"
#include "loader_start.h"
#include "on_path.h"
#include "own_run.h"
#include "paths.h"

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
    char *found = binary == NULL ? NULL : runtime_find_on_path(RUNTIME_HOST_IS_WINDOWS, binary, context->environment);
    if (found == NULL && binary != NULL) {
        label = label == NULL ? binary : label;
        const char *parts[5] = {"the ", label, " binary ", binary, " is not on the PATH"};
        size_t length = strlen(label) * 2 + strlen(binary) * 2 + 128;
        char *fix = malloc(length);
        if (fix != NULL) {
            snprintf(fix, length, "install %s so %s is on the PATH, or run forebay-host install --catalog <path or url> to install it",
                     label, binary);
        }
        *refusal = fix == NULL ? NULL : host_refusal_of(parts, 5, fix);
        free(fix);
    }
    return found;
}

/**
 * {host id: start} for every background host the loader's host.json names, and its session host on
 * javascript-host when its settings enable plugins of its own; NULL with *refusal set when one cannot be
 * started, before anything is.
 */
static cJSON *starts_of(const host_context *context, const char *loader_id, const loader_start *loader, cJSON **refusal) {
    cJSON *starts = cJSON_CreateObject();
    const cJSON *background = cJSON_GetObjectItemCaseSensitive(loader->standard, "background");
    for (const cJSON *id = starts == NULL || background == NULL ? NULL : background->child; id != NULL; id = id->next) {
        cJSON *start = cJSON_IsString(id) ? host_start_of(context, id->valuestring, refusal) : NULL;
        if (start == NULL || !cJSON_AddItemToObject(starts, id->valuestring, start)) {
            cJSON_Delete(start);
            cJSON_Delete(starts);
            return NULL;
        }
    }
    if (starts != NULL && cJSON_GetArraySize(loader->plugins) > 0) {
        cJSON *session = host_session_start_of(context, loader_id, refusal);
        if (session == NULL || !cJSON_AddItemToObject(starts, loader_id, session)) {
            cJSON_Delete(session);
            cJSON_Delete(starts);
            return NULL;
        }
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
    if (host_hold_all(holds->context, holds->starts, pid, &refusal)) {
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

static int run_held(const host_context *context, const char *binary, cJSON *starts, const cJSON *arguments) {
    cJSON *refusal = NULL;
    double self = host_process_id();
    if (!host_hold_all(context, starts, self, &refusal)) {
        return host_refuse(refusal);
    }
    launch_holds holds = {context, starts, 0, {NULL, NULL, NULL}};
    host_start_own_run(&holds.run, context);
    host_child_watch watch = {hold_for_application, stamp_own_run, HOST_OWN_RUN_INTERVAL_MILLISECONDS, &holds};
    int exit_code = 1;
    int ran = host_run_child(binary, arguments, context->environment, &watch, &exit_code, &refusal);
    host_finish_own_run(&holds.run);
    if (holds.application != 0) {
        host_release_all(context, starts, holds.application);
    }
    host_release_all(context, starts, self);
    return ran ? exit_code : host_refuse(refusal);
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
    cJSON *starts = binary == NULL ? NULL : starts_of(context, loader_id, &loader, &refusal);
    cJSON *passed = starts == NULL ? NULL : application_arguments(count, arguments);
    int code = passed == NULL ? host_refuse(refusal) : run_held(context, binary, starts, passed);
    cJSON_Delete(passed);
    cJSON_Delete(starts);
    free(binary);
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
