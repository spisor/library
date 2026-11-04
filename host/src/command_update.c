#include <string.h>

#include "boot_stub.h"
#include "file_times.h"
#include "host_commands.h"
#include "host_context.h"
#include "host_output.h"
#include "hosts.h"
#include "install_ecosystem.h"
#include "updates.h"

#define PATH_METHOD "path"
#define UPDATE_FIX "see each one's fix under refused above, then update again"

/** The registrations hosts.list answers, forebay-host first, so the catalog check it makes serves the rest. */
static cJSON *registrations_of(const host_context *context, cJSON **refusal) {
    cJSON *listed = host_call(runtime_hosts_list, host_places_argument(context), refusal);
    cJSON *registrations = listed == NULL ? NULL : cJSON_CreateArray();
    for (int pass = 0; registrations != NULL && pass < 2; pass++) {
        const cJSON *row;
        cJSON_ArrayForEach(row, cJSON_GetObjectItemCaseSensitive(listed, "hosts")) {
            const cJSON *registration = cJSON_GetObjectItemCaseSensitive(row, "registration");
            const char *id = host_text(registration, "id");
            int first = id != NULL && strcmp(id, RUNTIME_BOOTED_HOST_ID) == 0;
            if (id != NULL && first == (pass == 0)) {
                cJSON_AddItemToArray(registrations, cJSON_Duplicate(registration, 1));
            }
        }
    }
    cJSON_Delete(listed);
    return registrations;
}

static cJSON *check_call(const host_context *context, const cJSON *registration, cJSON **refusal) {
    cJSON *settings = host_settings_of(context, host_text(registration, "home"), refusal);
    cJSON *call = settings == NULL ? NULL : host_places_argument(context);
    int built = call != NULL && cJSON_AddStringToObject(call, "hostId", host_text(registration, "id")) != NULL &&
                cJSON_AddNumberToObject(call, "now", runtime_now_milliseconds()) != NULL &&
                cJSON_AddItemToObject(call, "settings", settings);
    if (!built) {
        cJSON_Delete(call);
        cJSON_Delete(settings);
        return NULL;
    }
    return call;
}

/** Whether the registration is a loader whose application an install method installed, which that method updates. */
static int has_method_application(const cJSON *registration) {
    const cJSON *provenance = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(registration, "application"),
                                                               "provenance");
    const char *method = host_text(provenance, "method");
    return method != NULL && strcmp(method, PATH_METHOD) != 0;
}

static cJSON *application_call(const host_context *context, const char *loader) {
    cJSON *call = host_places_argument(context);
    if (call != NULL && (cJSON_AddStringToObject(call, "loader", loader) == NULL ||
                         cJSON_AddStringToObject(call, "operation", "update") == NULL)) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

/** Adds what the call answered, or its refusal with the id it was for to refused; 0 when memory runs out. */
static int record(cJSON *answers, cJSON *refused, const char *id, cJSON *answer, cJSON *refusal) {
    if (answer != NULL) {
        return cJSON_AddItemToObject(answers, id, answer);
    }
    if (refusal == NULL || cJSON_AddStringToObject(refusal, "id", id) == NULL) {
        cJSON_Delete(refusal);
        return 0;
    }
    return cJSON_AddItemToArray(refused, refusal);
}

static int update_each(const host_context *context, const cJSON *registrations, cJSON *summary) {
    cJSON *checks = cJSON_AddObjectToObject(summary, "checks");
    cJSON *applications = cJSON_AddObjectToObject(summary, "applications");
    cJSON *refused = cJSON_AddArrayToObject(summary, "refused");
    int complete = checks != NULL && applications != NULL && refused != NULL;
    const cJSON *registration;
    cJSON_ArrayForEach(registration, registrations) {
        const char *id = host_text(registration, "id");
        cJSON *refusal = NULL;
        cJSON *checked = complete ? host_call(runtime_updates_check, check_call(context, registration, &refusal), &refusal) : NULL;
        complete = complete && record(checks, refused, id, checked, refusal);
        if (complete && has_method_application(registration)) {
            cJSON *updated = host_call(runtime_install_application, application_call(context, id), &refusal);
            complete = record(applications, refused, id, updated, refusal);
        }
    }
    return complete;
}

/**
 * Checks every registered host for updates, as each one's start would in the background, and updates each
 * application an install method installed through that method; prints {checks, applications, refused}.
 */
static int update_everything(const host_context *context) {
    cJSON *refusal = NULL;
    cJSON *registrations = registrations_of(context, &refusal);
    cJSON *summary = registrations == NULL ? NULL : cJSON_CreateObject();
    if (summary == NULL || !update_each(context, registrations, summary)) {
        cJSON_Delete(summary);
        cJSON_Delete(registrations);
        return host_refuse(refusal);
    }
    host_print_json(summary);
    int refused = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(summary, "refused")) > 0;
    cJSON_Delete(summary);
    cJSON_Delete(registrations);
    if (!refused) {
        return HOST_EXIT_DONE;
    }
    const char *parts[1] = {"not everything could be updated"};
    return host_refuse(host_refusal_of(parts, 1, UPDATE_FIX));
}

int host_command_update(int count, char **arguments) {
    (void)arguments;
    if (count != 0) {
        return host_misuse("update takes no arguments");
    }
    host_context context;
    cJSON *refusal = NULL;
    int code = host_open_context(&context, &refusal) ? update_everything(&context) : host_refuse(refusal);
    host_close_context(&context);
    return code;
}
