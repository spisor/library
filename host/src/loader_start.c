#include "loader_start.h"

#include <stdlib.h>
#include <string.h>

#include "install_ecosystem.h"
#include "loader.h"
#include "processes.h"
#include "updates.h"

static cJSON *apply_call(const host_context *context, const char *id, cJSON *settings) {
    cJSON *call = host_places_argument(context);
    if (call == NULL || cJSON_AddStringToObject(call, "hostId", id) == NULL || !cJSON_AddItemToObject(call, "settings", settings)) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

static void remove_run_record(const host_context *context, const char *id, const char *record) {
    cJSON *call = cJSON_CreateObject();
    if (call != NULL && (cJSON_AddStringToObject(call, "state", context->state) == NULL ||
                         cJSON_AddStringToObject(call, "hostId", id) == NULL ||
                         cJSON_AddStringToObject(call, "record", record) == NULL)) {
        cJSON_Delete(call);
        call = NULL;
    }
    cJSON *refusal = NULL;
    cJSON_Delete(host_call(runtime_processes_remove_runs, call, &refusal));
    cJSON_Delete(refusal);
}

/** updates.apply for id: the plugins its settings enable, and the version it runs into start. */
static cJSON *apply(const host_context *context, const char *id, loader_start *start, cJSON **refusal) {
    cJSON *settings = host_settings_of(context, host_text(start->registration, "home"), refusal);
    cJSON *applied = settings == NULL ? NULL : host_call(runtime_updates_apply, apply_call(context, id, settings), refusal);
    const char *record = host_text(applied, "runRecord");
    if (record != NULL) {
        remove_run_record(context, id, record);
    }
    return applied;
}

static cJSON *loader_call(const host_context *context, const char *id, const char *version, int with_environment) {
    cJSON *call = host_places_argument(context);
    int built = call != NULL && cJSON_AddStringToObject(call, "id", id) != NULL &&
                cJSON_AddStringToObject(call, "version", version) != NULL &&
                (!with_environment || cJSON_AddItemToObject(call, "environment", cJSON_Duplicate(context->environment, 1)));
    if (!built) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

int host_start_loader(const host_context *context, const char *id, loader_start *start, cJSON **refusal) {
    memset(start, 0, sizeof *start);
    start->registration = host_registration(context, id, refusal);
    cJSON *applied = start->registration == NULL ? NULL : apply(context, id, start, refusal);
    start->plugins = cJSON_DetachItemFromObjectCaseSensitive(applied, "plugins");
    const char *version = host_text(cJSON_GetObjectItemCaseSensitive(applied, "run"), id);
    start->version = version == NULL ? NULL : malloc(strlen(version) + 1);
    if (start->version != NULL) {
        strcpy(start->version, version);
    }
    cJSON_Delete(applied);
    cJSON *registered = start->version == NULL ? NULL
                                               : host_call(runtime_install_register_loader,
                                                           loader_call(context, id, start->version, 1), refusal);
    cJSON *installed = registered == NULL ? NULL
                                          : host_call(runtime_loader_installed, loader_call(context, id, start->version, 0),
                                                      refusal);
    cJSON_Delete(registered);
    start->standard = cJSON_DetachItemFromObjectCaseSensitive(installed, "standard");
    cJSON_Delete(installed);
    return start->standard != NULL;
}

void host_finish_loader(loader_start *start) {
    free(start->version);
    cJSON_Delete(start->standard);
    cJSON_Delete(start->plugins);
    cJSON_Delete(start->registration);
    memset(start, 0, sizeof *start);
}
