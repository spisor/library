#include <string.h>

#include "boot_stub.h"
#include "config.h"
#include "install_ecosystem.h"
#include "install_run.h"
#include "catalog_answer.h"
#include "json_service.h"
#include "registry.h"
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

/** Appends a copy of item to array; 0 when memory runs out. */
static int append_copy(cJSON *array, const cJSON *item) {
    cJSON *copy = cJSON_Duplicate(item, 1);
    if (copy == NULL || !cJSON_AddItemToArray(array, copy)) {
        cJSON_Delete(copy);
        return 0;
    }
    return 1;
}

/** The registrations hosts.list answers, spisor-host first, so the catalog check it makes serves the rest. */
static cJSON *registrations_of(const update_run *run, cJSON **refusal) {
    cJSON *listed = runtime_call_operation(runtime_registry_list_hosts, places_call(run), refusal);
    cJSON *registrations = listed == NULL ? NULL : cJSON_CreateArray();
    for (int pass = 0; registrations != NULL && pass < 2; pass++) {
        const cJSON *row;
        cJSON_ArrayForEach(row, cJSON_GetObjectItemCaseSensitive(listed, "hosts")) {
            const cJSON *registration = cJSON_GetObjectItemCaseSensitive(row, "registration");
            const char *id = text_of(registration, "id");
            if (registrations != NULL && id != NULL && (strcmp(id, RUNTIME_ECOSYSTEM_HOST_ID) == 0) == (pass == 0) &&
                !append_copy(registrations, registration)) {
                cJSON_Delete(registrations);
                registrations = NULL;
            }
        }
    }
    cJSON_Delete(listed);
    return registrations;
}

/** The settings values settings.all answers for a host's home; NULL with *refusal set when it refused. */
static cJSON *settings_of(const update_run *run, const char *home, cJSON **refusal) {
    return runtime_ecosystem_settings_values(home, text_of(run->places, "portable"), refusal);
}

/**
 * The updates.check call for registration: its own settings, which choose its plugins and pins, with the
 * catalog sources of ecosystem, spisor-host's settings.
 *
 * @implNote install records a local catalog source in spisor-host's own layer, which no other host reads,
 * so without this a loader would check no catalog at all.
 */
static cJSON *check_call(const update_run *run, const cJSON *registration, const cJSON *ecosystem, cJSON **refusal) {
    cJSON *settings = settings_of(run, text_of(registration, "home"), refusal);
    if (settings == NULL) {
        return NULL;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(settings, "catalog");
    const cJSON *catalog = cJSON_GetObjectItemCaseSensitive(ecosystem, "catalog");
    cJSON *call = places_call(run);
    int built = call != NULL &&
                (catalog == NULL || runtime_catalog_attach(settings, "catalog", cJSON_Duplicate(catalog, 1))) &&
                cJSON_AddStringToObject(call, "hostId", text_of(registration, "id")) != NULL &&
                cJSON_AddStringToObject(call, "now", run->now) != NULL;
    if (!built || !cJSON_AddItemToObject(call, "settings", settings)) {
        cJSON_Delete(settings);
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

/** install's application call updating the loader `id`; NULL when memory runs out. */
static cJSON *update_call(const update_run *run, const char *id) {
    cJSON *call = places_call(run);
    if (call == NULL || cJSON_AddStringToObject(call, "loader", id) == NULL ||
        cJSON_AddStringToObject(call, "operation", "update") == NULL) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

/** Whether the registration is a loader whose application an install method installed, which that method updates. */
static int has_method_application(const cJSON *registration) {
    const char *method = text_of(
        cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(registration, "application"), "provenance"), "method");
    return method != NULL && strcmp(method, PATH_METHOD) != 0;
}

static int add_answer(const update_run *run, const char *member, const char *id, cJSON *answer) {
    return cJSON_AddItemToObject(cJSON_GetObjectItemCaseSensitive(run->summary, member), id, answer);
}

static int add_refusal(const update_run *run, const char *id, cJSON *refusal) {
    return refusal != NULL && cJSON_AddStringToObject(refusal, "id", id) != NULL &&
           cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(run->summary, "refused"), refusal);
}

/**
 * Adds the answer under id to the summary's member, or the refusal with id to refused, deleting whichever it was
 * given when it cannot be added; 0 when memory runs out.
 */
static int record(const update_run *run, const char *member, const char *id, cJSON *answer, cJSON *refusal) {
    int added = answer != NULL ? add_answer(run, member, id, answer) : add_refusal(run, id, refusal);
    if (!added) {
        cJSON_Delete(answer != NULL ? answer : refusal);
    }
    return added;
}

/**
 * Checks one registration and updates its method-installed application; a host whose settings are
 * refused is recorded under refused and neither checked nor updated, since what it would check is unknown.
 */
static int update_one(const update_run *run, const cJSON *registration, const cJSON *ecosystem) {
    const char *id = text_of(registration, "id");
    cJSON *refusal = NULL;
    cJSON *check = check_call(run, registration, ecosystem, &refusal);
    int settings_read = check != NULL;
    cJSON *checked = settings_read ? runtime_call_operation(runtime_updates_check_operation, check, &refusal) : NULL;
    int complete = record(run, "checks", id, checked, refusal);
    if (complete && settings_read && has_method_application(registration)) {
        refusal = NULL;
        cJSON *updated = runtime_call_operation(runtime_install_application_operation, update_call(run, id), &refusal);
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

cJSON *runtime_updates_run_operation(const cJSON *argument, cJSON **refusal) {
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
    int ecosystem_first = first != NULL && strcmp(text_of(first, "id"), RUNTIME_ECOSYSTEM_HOST_ID) == 0;
    cJSON *ecosystem = registrations == NULL ? NULL
                       : ecosystem_first ? settings_of(&run, text_of(first, "home"), refusal)
                                         : cJSON_CreateObject();
    int complete = ecosystem != NULL;
    for (const cJSON *registration = complete ? registrations->child : NULL; complete && registration != NULL;
         registration = registration->next) {
        complete = update_one(&run, registration, ecosystem);
    }
    cJSON_Delete(ecosystem);
    cJSON_Delete(registrations);
    if (!complete) {
        cJSON_Delete(run.summary);
        return NULL;
    }
    return run.summary;
}
