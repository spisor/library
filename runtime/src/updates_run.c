#include <string.h>

#include "boot_stub.h"
#include "config.h"
#include "install_ecosystem.h"
#include "install_run.h"
#include "catalog_answer.h"
#include "host_references.h"
#include "json_service.h"
#include "platform_key.h"
#include "registry.h"
#include "updates.h"

#define PATH_METHOD "path"

typedef struct update_run {
    const char *portable;
    const char *state;
    const char *cache;
    cJSON *places;
    const char *now;
    cJSON *summary;
} update_run;

static const char *text_of(const cJSON *object, const char *name) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
}

/** {portable, state, cache}, the places install's application call takes; NULL when memory runs out. */
static cJSON *places_of(const char *portable, const char *state, const char *cache) {
    cJSON *places = cJSON_CreateObject();
    if (places == NULL || cJSON_AddStringToObject(places, "portable", portable) == NULL ||
        cJSON_AddStringToObject(places, "state", state) == NULL ||
        cJSON_AddStringToObject(places, "cache", cache) == NULL) {
        cJSON_Delete(places);
        return NULL;
    }
    return places;
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
    cJSON *listed = runtime_registry_list(run->state, refusal);
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
    return runtime_ecosystem_settings_values(home, run->portable, refusal);
}

/**
 * The settings updates.check takes for registration: its own, which choose its plugins and pins, with the catalog
 * sources of ecosystem, spisor-host's settings.
 *
 * @implNote install records a local catalog source in spisor-host's own layer, which no other host reads,
 * so without this a loader would check no catalog at all.
 */
static cJSON *check_settings(const update_run *run, const cJSON *registration, const cJSON *ecosystem,
                             cJSON **refusal) {
    cJSON *settings = settings_of(run, text_of(registration, "home"), refusal);
    if (settings == NULL) {
        return NULL;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(settings, "catalog");
    const cJSON *catalog = cJSON_GetObjectItemCaseSensitive(ecosystem, "catalog");
    if (catalog != NULL && !runtime_catalog_attach(settings, "catalog", cJSON_Duplicate(catalog, 1))) {
        cJSON_Delete(settings);
        return NULL;
    }
    return settings;
}

/** Checks the host id, with its settings; NULL with *refusal set when it refused, or when memory runs out. */
static cJSON *checked(const update_run *run, const char *id, const cJSON *settings, cJSON **refusal) {
    runtime_updates_check_request request = {
        {run->state, run->cache, id, runtime_platform_key()}, run->now, settings, NULL, NULL};
    if (!runtime_is_plain_host_id(id)) {
        *refusal = runtime_refusal("the host id is not a plain file name", RUNTIME_PLAIN_HOST_ID_FIX);
        return NULL;
    }
    return runtime_updates_check(&request, refusal);
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
    cJSON *settings = check_settings(run, registration, ecosystem, &refusal);
    int settings_read = settings != NULL;
    cJSON *check = settings_read ? checked(run, id, settings, &refusal) : NULL;
    int complete = record(run, "checks", id, check, refusal);
    cJSON_Delete(settings);
    if (complete && settings_read && has_method_application(registration)) {
        refusal = NULL;
        cJSON *updated = runtime_install_operate_application(run->places, id, "update", &refusal);
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

cJSON *runtime_updates_run(const char *portable, const char *state, const char *cache, const char *now,
                          cJSON **refusal) {
    update_run run = {portable, state, cache, places_of(portable, state, cache), now, NULL};
    *refusal = NULL;
    run.summary = run.places == NULL ? NULL : summary_of();
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
    cJSON_Delete(run.places);
    if (!complete) {
        cJSON_Delete(run.summary);
        return NULL;
    }
    return run.summary;
}
