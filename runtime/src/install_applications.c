#include <stdlib.h>
#include <string.h>

#include "cache_refusal.h"
#include "catalog_text.h"
#include "install.h"
#include "install_methods.h"
#include "install_run.h"
#include "install_support.h"
#include "json_service.h"
#include "lists.h"

#define SKIP "skip"
#define PATH_METHOD "path"

/** One catalog application still to settle: what it can be installed through and what it cannot. */
typedef struct pending_application {
    const cJSON *application;
    cJSON *offered;
    cJSON *unavailable;
} pending_application;

static const char *application_id(const cJSON *application) {
    return runtime_install_text(application, "id");
}

/** Every catalog application, borrowed, in catalog order; NULL when memory runs out. */
static cJSON *applications_of(const cJSON *catalog) {
    cJSON *applications = cJSON_CreateArray();
    for (const cJSON *item = cJSON_GetObjectItemCaseSensitive(catalog, "items")->child; applications != NULL && item != NULL;
         item = item->next) {
        const cJSON *application = cJSON_GetObjectItemCaseSensitive(item, "application");
        const char *kind = runtime_install_text(item, "kind");
        if (kind != NULL && strcmp(kind, "host") == 0 && cJSON_IsObject(application) && application_id(application) != NULL &&
            !cJSON_AddItemReferenceToArray(applications, (cJSON *)application)) {
            cJSON_Delete(applications);
            applications = NULL;
        }
    }
    return applications;
}

static cJSON *all_detections(const runtime_install_run *run, const cJSON *applications, cJSON **refusal) {
    cJSON *detections = cJSON_CreateArray();
    for (const cJSON *application = detections == NULL ? NULL : applications->child; application != NULL;
         application = application->next) {
        cJSON *found = runtime_install_detections(run->places, application_id(application),
                                                  cJSON_GetObjectItemCaseSensitive(application, "packages"), refusal);
        while (found != NULL && found->child != NULL) {
            cJSON_AddItemToArray(detections, cJSON_DetachItemViaPointer(found, found->child));
        }
        if (found == NULL) {
            cJSON_Delete(detections);
            return NULL;
        }
        cJSON_Delete(found);
    }
    return detections;
}

static cJSON *states_of(const runtime_install_run *run, const cJSON *detections, cJSON **refusal) {
    cJSON *call = runtime_install_places_call(run->places);
    if (call == NULL || !runtime_install_copy_into(call, "catalog", run->catalog) ||
        !runtime_install_copy_into(call, "environment", run->environment) ||
        !runtime_install_copy_into(call, "detections", detections)) {
        cJSON_Delete(call);
        return NULL;
    }
    return runtime_call_or_refuse(runtime_lists_applications, call, refusal);
}

static const char *state_of(const cJSON *states, const char *id) {
    for (const cJSON *row = cJSON_GetObjectItemCaseSensitive(states, "items")->child; row != NULL; row = row->next) {
        const char *row_id = runtime_install_text(row, "id");
        if (row_id != NULL && strcmp(row_id, id) == 0) {
            return runtime_install_text(row, "state");
        }
    }
    return NULL;
}

static int is_settled(const char *state) {
    return state != NULL && (strcmp(state, "installed") == 0 || strcmp(state, "detected") == 0);
}

static cJSON *mapping_of(const runtime_install_run *run, const cJSON *application, cJSON **refusal) {
    cJSON *call = runtime_install_places_call(run->places);
    if (call == NULL || cJSON_AddStringToObject(call, "item", application_id(application)) == NULL ||
        !runtime_install_copy_into(call, "manifestPackages", cJSON_GetObjectItemCaseSensitive(application, "packages"))) {
        cJSON_Delete(call);
        return NULL;
    }
    cJSON *answer = runtime_call_or_refuse(runtime_install_packages, call, refusal);
    cJSON *packages = cJSON_DetachItemFromObjectCaseSensitive(answer, "packages");
    cJSON_Delete(answer);
    return packages;
}

static int add_unavailable_method(cJSON *unavailable, const char *method) {
    const char *parts[3] = {"this runtime has no ", method, " install method built in"};
    cJSON *row = cJSON_CreateObject();
    char *message = runtime_cache_text(parts, 3);
    int added = row != NULL && message != NULL && cJSON_AddStringToObject(row, "method", method) != NULL &&
                cJSON_AddStringToObject(row, "message", message) != NULL &&
                cJSON_AddStringToObject(row, "fix", "choose another method, or install it yourself and run install again") != NULL &&
                cJSON_AddItemToArray(unavailable, row);
    free(message);
    if (!added) {
        cJSON_Delete(row);
    }
    return added;
}

/** @implNote marketplace is never offered: it installs a plugin from a marketplace, and no method can carry it out for an application. */
static int offer_for(const runtime_install_run *run, pending_application *pending, cJSON **refusal) {
    cJSON *packages = mapping_of(run, pending->application, refusal);
    pending->offered = packages == NULL ? NULL : cJSON_CreateArray();
    pending->unavailable = pending->offered == NULL ? NULL : cJSON_CreateArray();
    int offered = pending->unavailable != NULL;
    for (const cJSON *method = offered ? packages->child : NULL; offered && method != NULL; method = method->next) {
        offered = runtime_install_method_named(method->string) != NULL
                      ? runtime_install_add_text(pending->offered, method->string)
                      : add_unavailable_method(pending->unavailable, method->string);
    }
    cJSON_Delete(packages);
    return offered;
}

static char *joined_names(const cJSON *names, const char *extra) {
    runtime_catalog_text text;
    runtime_catalog_text_start(&text);
    for (const cJSON *name = names->child; name != NULL; name = name->next) {
        runtime_catalog_text_add(&text, text.length == 0 ? "" : ", ");
        runtime_catalog_text_add(&text, name->valuestring);
    }
    if (extra != NULL) {
        runtime_catalog_text_add(&text, text.length == 0 ? "" : ", ");
        runtime_catalog_text_add(&text, extra);
    }
    if (text.length == 0) {
        runtime_catalog_text_add(&text, "(none)");
    }
    return runtime_catalog_text_finish(&text);
}

static cJSON *refuse_unknown(const char *id, const cJSON *applications) {
    cJSON *ids = cJSON_CreateArray();
    for (const cJSON *application = ids == NULL ? NULL : applications->child; application != NULL; application = application->next) {
        runtime_install_add_text(ids, application_id(application));
    }
    char *known = ids == NULL ? NULL : joined_names(ids, NULL);
    const char *parts[4] = {"the choices name ", id, ", which the catalog lists no application under; the applications are ",
                            known};
    cJSON *refusal = known == NULL ? NULL : runtime_cache_refusal(parts, 4, "choose one of the catalog's applications");
    free(known);
    cJSON_Delete(ids);
    return refusal;
}

static int knows_every_choice(const runtime_install_run *run, const cJSON *applications, cJSON **refusal) {
    for (const cJSON *choice = run->choices == NULL ? NULL : run->choices->child; choice != NULL; choice = choice->next) {
        int known = 0;
        for (const cJSON *application = applications->child; !known && application != NULL; application = application->next) {
            known = strcmp(application_id(application), choice->string) == 0;
        }
        if (!known) {
            *refusal = refuse_unknown(choice->string, applications);
            return 0;
        }
    }
    return 1;
}

static const char *choice_of(const runtime_install_run *run, const char *id) {
    return runtime_install_text(run->choices, id);
}

static int is_offered_choice(const runtime_install_run *run, const pending_application *pending, cJSON **refusal) {
    const char *id = application_id(pending->application);
    const char *method = choice_of(run, id);
    if (method == NULL || strcmp(method, SKIP) == 0 || runtime_install_holds(pending->offered, method)) {
        return 1;
    }
    char *offered = joined_names(pending->offered, SKIP);
    const char *parts[6] = {"the choice ", id, "=", method, " names a method that is not offered; the offered methods are ",
                            offered};
    *refusal = offered == NULL ? NULL : runtime_cache_refusal(parts, 6, "choose one of the offered methods");
    free(offered);
    return 0;
}

static cJSON *row_head(const char *id, const char *state) {
    cJSON *row = cJSON_CreateObject();
    if (row == NULL || cJSON_AddStringToObject(row, "id", id) == NULL ||
        cJSON_AddStringToObject(row, "state", state) == NULL) {
        cJSON_Delete(row);
        return NULL;
    }
    return row;
}

/** {method, package, options?, version?} of a detection's installed answer, or NULL. */
static cJSON *origin_of_detection(const cJSON *detection) {
    const cJSON *answer = cJSON_GetObjectItemCaseSensitive(detection, "answer");
    cJSON *origin = cJSON_CreateObject();
    if (origin == NULL || !runtime_install_copy_into(origin, "method", cJSON_GetObjectItemCaseSensitive(detection, "method")) ||
        !runtime_install_copy_into(origin, "package", cJSON_GetObjectItemCaseSensitive(detection, "package")) ||
        !runtime_install_copy_into(origin, "options", cJSON_GetObjectItemCaseSensitive(detection, "options")) ||
        !runtime_install_copy_into(origin, "version", cJSON_GetObjectItemCaseSensitive(answer, "version"))) {
        cJSON_Delete(origin);
        return NULL;
    }
    return origin;
}

/** @implNote A method's answer wins over PATH, so an application a method reports is recorded under that method. */
static cJSON *detected_origin(const char *id, const cJSON *detections) {
    for (const cJSON *detection = detections->child; detection != NULL; detection = detection->next) {
        const char *item = runtime_install_text(detection, "item");
        if (item != NULL && strcmp(item, id) == 0 &&
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(detection, "answer"), "installed"))) {
            return origin_of_detection(detection);
        }
    }
    return cJSON_Parse("{\"method\": \"" PATH_METHOD "\"}");
}

static cJSON *settled_row(const char *id, const char *state, const cJSON *detections) {
    cJSON *row = row_head(id, state);
    if (row != NULL && strcmp(state, "detected") == 0 && !cJSON_AddItemToObject(row, "origin", detected_origin(id, detections))) {
        cJSON_Delete(row);
        return NULL;
    }
    return row;
}

static cJSON *with_unavailable(cJSON *row, const pending_application *pending) {
    if (row != NULL && cJSON_GetArraySize(pending->unavailable) > 0 &&
        !runtime_install_copy_into(row, "unavailable", pending->unavailable)) {
        cJSON_Delete(row);
        return NULL;
    }
    return row;
}

static cJSON *failed_row(const char *id, const char *method, cJSON *refusal) {
    cJSON *row = runtime_is_refusal(refusal) ? row_head(id, "failed") : NULL;
    cJSON *unavailable = row == NULL ? NULL : cJSON_AddArrayToObject(row, "unavailable");
    cJSON *entry = unavailable == NULL ? NULL : cJSON_CreateObject();
    int built = entry != NULL && cJSON_AddStringToObject(entry, "method", method) != NULL &&
                runtime_install_copy_into(entry, "message", cJSON_GetObjectItemCaseSensitive(refusal, "message")) &&
                runtime_install_copy_into(entry, "fix", cJSON_GetObjectItemCaseSensitive(refusal, "fix")) &&
                cJSON_AddItemToArray(unavailable, entry);
    cJSON_Delete(refusal);
    if (!built) {
        cJSON_Delete(entry);
        cJSON_Delete(row);
        return NULL;
    }
    return row;
}

static cJSON *plan_call(const runtime_install_run *run, const pending_application *pending, const char *method) {
    cJSON *call = runtime_install_places_call(run->places);
    if (call == NULL || cJSON_AddStringToObject(call, "item", application_id(pending->application)) == NULL ||
        !runtime_install_copy_into(call, "manifestPackages", cJSON_GetObjectItemCaseSensitive(pending->application, "packages")) ||
        !runtime_install_copy_into(call, "methods", pending->offered) || cJSON_AddStringToObject(call, "chosen", method) == NULL ||
        !runtime_install_copy_into(call, "installedProviders", pending->offered)) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

/** The origin an install through method records: the package and what the method reports after it. */
static cJSON *installed_origin(const runtime_install_run *run, const char *id, const char *method, const cJSON *mapped) {
    cJSON *detection = runtime_install_detection(run->places, id, runtime_install_method_named(method), mapped);
    cJSON *origin = cJSON_CreateObject();
    const cJSON *answer = cJSON_GetObjectItemCaseSensitive(detection, "answer");
    const cJSON *version = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answer, "installed"))
                               ? cJSON_GetObjectItemCaseSensitive(answer, "version")
                               : NULL;
    if (detection == NULL || origin == NULL || cJSON_AddStringToObject(origin, "method", method) == NULL ||
        !runtime_install_copy_into(origin, "package", cJSON_GetObjectItemCaseSensitive(mapped, "name")) ||
        !runtime_install_copy_into(origin, "options", cJSON_GetObjectItemCaseSensitive(mapped, "options")) ||
        !runtime_install_copy_into(origin, "version", version)) {
        cJSON_Delete(origin);
        origin = NULL;
    }
    cJSON_Delete(detection);
    return origin;
}

static cJSON *installed_row(const char *id, const char *method, cJSON *origin) {
    cJSON *row = origin == NULL ? NULL : row_head(id, "installed");
    if (row == NULL || cJSON_AddStringToObject(row, "installedThrough", method) == NULL ||
        !cJSON_AddItemToObject(row, "origin", origin)) {
        if (row == NULL || cJSON_GetObjectItemCaseSensitive(row, "origin") == NULL) {
            cJSON_Delete(origin);
        }
        cJSON_Delete(row);
        return NULL;
    }
    return row;
}

/** Carries out each install step of the plan through method; the row of what happened. */
static cJSON *install_through(const runtime_install_run *run, const pending_application *pending, const char *method) {
    const char *id = application_id(pending->application);
    cJSON *refusal = NULL;
    cJSON *plan = runtime_call_or_refuse(runtime_install_plan, plan_call(run, pending, method), &refusal);
    if (plan == NULL) {
        return refusal == NULL ? NULL : failed_row(id, method, refusal);
    }
    const cJSON *mapped = NULL;
    for (const cJSON *step = cJSON_GetObjectItemCaseSensitive(plan, "steps")->child; step != NULL; step = step->next) {
        const cJSON *package = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(step, "install"), "package");
        if (package != NULL && !runtime_install_operate(run->places, method, "install", package, &refusal)) {
            cJSON_Delete(plan);
            return refusal == NULL ? NULL : failed_row(id, method, refusal);
        }
        mapped = package != NULL ? package : mapped;
    }
    cJSON *origin = mapped == NULL ? cJSON_CreateObject() : installed_origin(run, id, method, mapped);
    if (mapped == NULL && origin != NULL && cJSON_AddStringToObject(origin, "method", method) == NULL) {
        cJSON_Delete(origin);
        origin = NULL;
    }
    cJSON_Delete(plan);
    return installed_row(id, method, origin);
}

static int add_missing(cJSON *report, const char *id, const cJSON *offered) {
    cJSON *entry = cJSON_CreateObject();
    if (entry == NULL || cJSON_AddStringToObject(entry, "application", id) == NULL ||
        !runtime_install_copy_into(entry, "methods", offered) ||
        !cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(report, "missing"), entry)) {
        cJSON_Delete(entry);
        return 0;
    }
    return 1;
}

static cJSON *missing_row(const runtime_install_run *run, const pending_application *pending) {
    const char *id = application_id(pending->application);
    const char *parts[4] = {"--application ", id, "=", pending->offered->child->valuestring};
    char *flag = runtime_cache_text(parts, 4);
    cJSON *row = flag == NULL ? NULL : row_head(id, "missing");
    if (row == NULL || !runtime_install_copy_into(row, "offered", pending->offered) ||
        cJSON_AddStringToObject(row, "flag", flag) == NULL || !add_missing(run->report, id, pending->offered)) {
        cJSON_Delete(row);
        row = NULL;
    }
    free(flag);
    return row;
}

static cJSON *resolved_row(const runtime_install_run *run, const pending_application *pending) {
    const char *id = application_id(pending->application);
    const char *method = choice_of(run, id);
    if (method == NULL) {
        return with_unavailable(cJSON_GetArraySize(pending->offered) == 0 ? row_head(id, "unavailable") : missing_row(run, pending),
                                pending);
    }
    if (strcmp(method, SKIP) == 0) {
        return with_unavailable(row_head(id, "skipped"), pending);
    }
    return with_unavailable(install_through(run, pending, method), pending);
}

static void discard_pending(pending_application *pending, size_t count) {
    for (size_t index = 0; pending != NULL && index < count; index++) {
        cJSON_Delete(pending[index].offered);
        cJSON_Delete(pending[index].unavailable);
    }
    free(pending);
}

/** The offers of every application that is neither installed nor detected, each choice checked against them. */
static pending_application *pending_of(const runtime_install_run *run, const cJSON *applications, const cJSON *states,
                                       cJSON **refusal) {
    size_t count = (size_t)cJSON_GetArraySize(applications);
    pending_application *pending = calloc(count + 1, sizeof *pending);
    size_t index = 0;
    for (const cJSON *application = pending == NULL ? NULL : applications->child; application != NULL;
         application = application->next, index++) {
        pending[index].application = application;
        if (is_settled(state_of(states, application_id(application)))) {
            continue;
        }
        if (!offer_for(run, &pending[index], refusal) || !is_offered_choice(run, &pending[index], refusal)) {
            discard_pending(pending, count);
            return NULL;
        }
    }
    return pending;
}

static int settle_all(runtime_install_run *run, const cJSON *applications, const cJSON *states, const cJSON *detections,
                      const pending_application *pending) {
    cJSON *rows = cJSON_GetObjectItemCaseSensitive(run->report, "applications");
    size_t index = 0;
    for (const cJSON *application = applications->child; application != NULL; application = application->next, index++) {
        const char *id = application_id(application);
        const char *state = state_of(states, id);
        cJSON *row = pending[index].offered == NULL ? settled_row(id, is_settled(state) ? state : "installed", detections)
                                                    : resolved_row(run, &pending[index]);
        if (row == NULL || !cJSON_AddItemToArray(rows, row)) {
            cJSON_Delete(row);
            return 0;
        }
    }
    return 1;
}

int runtime_install_resolve_applications(runtime_install_run *run, cJSON **refusal) {
    cJSON *applications = applications_of(run->catalog);
    cJSON *detections = applications == NULL || !knows_every_choice(run, applications, refusal)
                            ? NULL
                            : all_detections(run, applications, refusal);
    cJSON *states = detections == NULL ? NULL : states_of(run, detections, refusal);
    pending_application *pending = states == NULL ? NULL : pending_of(run, applications, states, refusal);
    int settled = pending != NULL && settle_all(run, applications, states, detections, pending);
    discard_pending(pending, applications == NULL ? 0 : (size_t)cJSON_GetArraySize(applications));
    cJSON_Delete(states);
    cJSON_Delete(detections);
    cJSON_Delete(applications);
    return settled;
}
