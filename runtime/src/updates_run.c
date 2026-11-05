#include <string.h>

#include "boot_stub.h"
#include "config.h"
#include "hosts.h"
#include "install_ecosystem.h"
#include "json_service.h"
#include "updates.h"

#define PATH_METHOD "path"
#define RUN_FIX "send {\"places\": {\"portable\", \"state\", \"cache\"}, \"now\": \"<ISO time>\"}"

typedef struct update_run {
    const cJSON *places;
    const char *now;
    cJSON *summary;
} update_run;

static const char *text_of(const cJSON *object, const char *name) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
}

static cJSON *places_call(const update_run *run) {
    cJSON *call = cJSON_CreateObject();
    cJSON *places = cJSON_Duplicate(run->places, 1);
    if (call == NULL || places == NULL || !cJSON_AddItemToObject(call, "places", places)) {
        cJSON_Delete(places);
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

/** The registrations hosts.list answers, forebay-host first, so the catalog check it makes serves the rest. */
static cJSON *registrations_of(const update_run *run, cJSON **refusal) {
    cJSON *listed = runtime_call_or_refuse(runtime_hosts_list, places_call(run), refusal);
    cJSON *registrations = listed == NULL ? NULL : cJSON_CreateArray();
    for (int pass = 0; registrations != NULL && pass < 2; pass++) {
        const cJSON *row;
        cJSON_ArrayForEach(row, cJSON_GetObjectItemCaseSensitive(listed, "hosts")) {
            const cJSON *registration = cJSON_GetObjectItemCaseSensitive(row, "registration");
            const char *id = text_of(registration, "id");
            if (id != NULL && (strcmp(id, RUNTIME_BOOTED_HOST_ID) == 0) == (pass == 0)) {
                cJSON_AddItemToArray(registrations, cJSON_Duplicate(registration, 1));
            }
        }
    }
    cJSON_Delete(listed);
    return registrations;
}

/** The settings values settings.all answers for a host's home; an empty object when there are none. */
static cJSON *settings_of(const update_run *run, const char *home) {
    cJSON *call = cJSON_CreateObject();
    cJSON_AddStringToObject(call, "home", home == NULL ? "" : home);
    cJSON_AddStringToObject(call, "portable", text_of(run->places, "portable"));
    cJSON *refusal = NULL;
    cJSON *answer = runtime_call_or_refuse(runtime_settings_all, call, &refusal);
    cJSON_Delete(refusal);
    cJSON *values = cJSON_DetachItemFromObjectCaseSensitive(answer, "values");
    cJSON_Delete(answer);
    return values == NULL ? cJSON_CreateObject() : values;
}

/**
 * The updates.check call for registration: its own settings, which choose its plugins and pins, with the
 * catalog sources of ecosystem, forebay-host's settings.
 *
 * @implNote install records a local catalog source in forebay-host's own layer, which no other host reads,
 * so without this a loader would check no catalog at all.
 */
static cJSON *check_call(const update_run *run, const cJSON *registration, const cJSON *ecosystem) {
    cJSON *settings = settings_of(run, text_of(registration, "home"));
    cJSON_DeleteItemFromObjectCaseSensitive(settings, "catalog");
    const cJSON *catalog = cJSON_GetObjectItemCaseSensitive(ecosystem, "catalog");
    if (catalog != NULL) {
        cJSON_AddItemToObject(settings, "catalog", cJSON_Duplicate(catalog, 1));
    }
    cJSON *call = places_call(run);
    cJSON_AddStringToObject(call, "hostId", text_of(registration, "id"));
    cJSON_AddStringToObject(call, "now", run->now);
    cJSON_AddItemToObject(call, "settings", settings);
    return call;
}

/** Whether the registration is a loader whose application an install method installed, which that method updates. */
static int has_method_application(const cJSON *registration) {
    const char *method = text_of(
        cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(registration, "application"), "provenance"), "method");
    return method != NULL && strcmp(method, PATH_METHOD) != 0;
}

/** Adds the answer under id to the summary's member, or the refusal with id to refused; 0 when memory runs out. */
static int record(const update_run *run, const char *member, const char *id, cJSON *answer, cJSON *refusal) {
    if (answer != NULL) {
        return cJSON_AddItemToObject(cJSON_GetObjectItemCaseSensitive(run->summary, member), id, answer);
    }
    if (refusal == NULL || cJSON_AddStringToObject(refusal, "id", id) == NULL) {
        cJSON_Delete(refusal);
        return 0;
    }
    return cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(run->summary, "refused"), refusal);
}

static int update_one(const update_run *run, const cJSON *registration, const cJSON *ecosystem) {
    const char *id = text_of(registration, "id");
    cJSON *refusal = NULL;
    cJSON *checked = runtime_call_or_refuse(runtime_updates_check, check_call(run, registration, ecosystem), &refusal);
    int complete = record(run, "checks", id, checked, refusal);
    if (complete && has_method_application(registration)) {
        cJSON *call = places_call(run);
        cJSON_AddStringToObject(call, "loader", id);
        cJSON_AddStringToObject(call, "operation", "update");
        refusal = NULL;
        cJSON *updated = runtime_call_or_refuse(runtime_install_application, call, &refusal);
        complete = record(run, "applications", id, updated, refusal);
    }
    return complete;
}

static cJSON *summary_of(void) {
    cJSON *summary = cJSON_CreateObject();
    if (summary == NULL || cJSON_AddObjectToObject(summary, "checks") == NULL ||
        cJSON_AddObjectToObject(summary, "applications") == NULL || cJSON_AddArrayToObject(summary, "refused") == NULL) {
        cJSON_Delete(summary);
        return NULL;
    }
    return summary;
}

static cJSON *run_updates(const cJSON *argument, cJSON **refusal) {
    const cJSON *places = cJSON_GetObjectItemCaseSensitive(argument, "places");
    const char *now = text_of(argument, "now");
    if (!cJSON_IsObject(places) || text_of(places, "portable") == NULL || text_of(places, "state") == NULL ||
        text_of(places, "cache") == NULL || now == NULL || now[0] == 0) {
        *refusal = runtime_refusal("the argument carries no usable places or now", RUN_FIX);
        return NULL;
    }
    update_run run = {places, now, summary_of()};
    cJSON *registrations = run.summary == NULL ? NULL : registrations_of(&run, refusal);
    const cJSON *first = cJSON_GetArrayItem(registrations, 0);
    int ecosystem_first = first != NULL && strcmp(text_of(first, "id"), RUNTIME_BOOTED_HOST_ID) == 0;
    cJSON *ecosystem = registrations == NULL ? NULL : ecosystem_first ? settings_of(&run, text_of(first, "home")) : cJSON_CreateObject();
    int complete = ecosystem != NULL;
    const cJSON *registration;
    cJSON_ArrayForEach(registration, complete ? registrations : NULL) {
        complete = complete && update_one(&run, registration, ecosystem);
    }
    cJSON_Delete(ecosystem);
    cJSON_Delete(registrations);
    if (!complete) {
        cJSON_Delete(run.summary);
        return NULL;
    }
    return run.summary;
}

RUNTIME_EXPORT int32_t runtime_updates_run(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, run_updates);
}
