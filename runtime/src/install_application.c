#include "install_ecosystem.h"

#include <stdlib.h>
#include <string.h>

#include "cache_refusal.h"
#include "install_methods.h"
#include "install_support.h"
#include "json_service.h"
#include "registry.h"

#define APPLICATION_FIX                                                                                       \
    "send {\"places\": {\"portable\", \"state\", \"cache\"}, \"loader\": \"<loader id>\", \"operation\": " \
    "\"update\" | \"remove\"}"

typedef struct application_operation {
    const cJSON *places;
    const char *loader;
    const char *operation;
} application_operation;

static cJSON *registration_of(const application_operation *request, cJSON **refusal) {
    cJSON *call = runtime_install_places_call(request->places);
    if (call != NULL && cJSON_AddStringToObject(call, "id", request->loader) == NULL) {
        cJSON_Delete(call);
        call = NULL;
    }
    cJSON *answer = runtime_call_or_refuse(runtime_registry_get, call, refusal);
    cJSON *registration = cJSON_DetachItemFromObjectCaseSensitive(answer, "registration");
    cJSON_Delete(answer);
    return registration;
}

static cJSON *refused(const char *const *parts, size_t count, const char *fix_verb, const char *application) {
    const char *fix_parts[3] = {fix_verb, application, " with whatever installed it"};
    char *fix = runtime_cache_text(fix_parts, 3);
    cJSON *refusal = fix == NULL ? NULL : runtime_cache_refusal(parts, count, fix);
    free(fix);
    return refusal;
}

/** @implNote A route that is path or missing was never an install method's to change, so it says where the application came from rather than guessing. */
static cJSON *route_refusal(const cJSON *application, const char *operation) {
    const char *id = runtime_install_text(application, "id");
    const cJSON *provenance = cJSON_GetObjectItemCaseSensitive(application, "provenance");
    const char *method = runtime_install_text(provenance, "method");
    const char *verb = strcmp(operation, "update") == 0 ? "update " : "remove ";
    if (method == NULL) {
        const char *parts[3] = {"how ", id, " was installed is not recorded"};
        return refused(parts, 3, verb, id);
    }
    if (strcmp(method, "path") == 0) {
        const char *location = runtime_install_text(provenance, "location");
        const char *parts[4] = {id, " was found on this machine at ", location == NULL ? "an unrecorded place" : location,
                                ", not installed through an install method"};
        return refused(parts, 4, verb, id);
    }
    if (runtime_install_text(provenance, "package") == NULL) {
        const char *parts[5] = {"the record of ", id, " names method ", method, " and no package"};
        return refused(parts, 5, verb, id);
    }
    return NULL;
}

static cJSON *recorded_application(const application_operation *request, cJSON **registration, cJSON **refusal) {
    *registration = registration_of(request, refusal);
    if (*registration == NULL) {
        if (*refusal == NULL) {
            const char *parts[2] = {"no host is registered as ", request->loader};
            *refusal = runtime_cache_refusal(parts, 2, "name a registered loader");
        }
        return NULL;
    }
    cJSON *application = cJSON_GetObjectItemCaseSensitive(*registration, "application");
    if (!cJSON_IsObject(application) || runtime_install_text(application, "id") == NULL) {
        const char *parts[2] = {request->loader, " is not a loader of an application"};
        *refusal = runtime_cache_refusal(parts, 2, "name the loader of the application");
        return NULL;
    }
    *refusal = route_refusal(application, request->operation);
    return *refusal == NULL ? application : NULL;
}

static cJSON *mapped_of(const cJSON *provenance) {
    cJSON *mapped = cJSON_CreateObject();
    if (mapped == NULL || !runtime_install_copy_into(mapped, "name", cJSON_GetObjectItemCaseSensitive(provenance, "package")) ||
        !runtime_install_copy_into(mapped, "options", cJSON_GetObjectItemCaseSensitive(provenance, "options"))) {
        cJSON_Delete(mapped);
        return NULL;
    }
    return mapped;
}

/** The version the method reports for the package now, or NULL when it reports none. */
static char *reported_version(const application_operation *request, const cJSON *application, const char *method,
                              const cJSON *mapped) {
    const runtime_install_method *found = runtime_install_method_named(method);
    cJSON *detection = found == NULL ? NULL
                                     : runtime_install_detection(request->places, runtime_install_text(application, "id"),
                                                                 found, mapped);
    const cJSON *answer = cJSON_GetObjectItemCaseSensitive(detection, "answer");
    const char *version = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answer, "installed"))
                              ? runtime_install_text(answer, "version")
                              : NULL;
    char *copy = version == NULL ? NULL : malloc(strlen(version) + 1);
    if (copy != NULL) {
        strcpy(copy, version);
    }
    cJSON_Delete(detection);
    return copy;
}

/**
 * @implNote registry.register replaces the registration's application whole, so the registration is
 * read again right before the write and only application.provenance.version differs from it; a change
 * made since the operation began is kept.
 */
static int record_version(const application_operation *request, const char *version) {
    cJSON *refusal = NULL;
    cJSON *registration = registration_of(request, &refusal);
    cJSON_Delete(refusal);
    refusal = NULL;
    cJSON *provenance = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(registration, "application"), "provenance");
    cJSON_DeleteItemFromObjectCaseSensitive(provenance, "version");
    cJSON *call = provenance == NULL || cJSON_AddStringToObject(provenance, "version", version) == NULL
                      ? NULL
                      : runtime_install_places_call(request->places);
    int built = call != NULL && cJSON_AddItemToObject(call, "registration", registration);
    if (!built) {
        cJSON_Delete(registration);
        cJSON_Delete(call);
        return 0;
    }
    cJSON *registered = runtime_call_or_refuse(runtime_registry_register, call, &refusal);
    cJSON_Delete(refusal);
    cJSON_Delete(registered);
    return registered != NULL;
}

/**
 * @implNote An update the method carried out is done whether or not its version could be recorded; the
 * unrecorded version is noted so the caller can say so.
 */
static cJSON *answer_of(const char *operation, const char *version, int recorded) {
    cJSON *answer = cJSON_CreateObject();
    cJSON *unrecorded = recorded ? NULL
                                 : runtime_refusal("the updated version could not be recorded in the loader's registration",
                                                   "run install again to record how the application was installed");
    if (answer == NULL || cJSON_AddStringToObject(answer, "operation", operation) == NULL ||
        (version != NULL && cJSON_AddStringToObject(answer, "version", version) == NULL) ||
        (!recorded && (unrecorded == NULL || !cJSON_AddItemToObject(answer, "unrecorded", unrecorded)))) {
        if (cJSON_GetObjectItemCaseSensitive(answer, "unrecorded") == NULL) {
            cJSON_Delete(unrecorded);
        }
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static cJSON *operate_through_record(const application_operation *request, cJSON **refusal) {
    cJSON *registration = NULL;
    const cJSON *application = recorded_application(request, &registration, refusal);
    const cJSON *provenance = cJSON_GetObjectItemCaseSensitive(application, "provenance");
    const char *method = runtime_install_text(provenance, "method");
    cJSON *mapped = application == NULL ? NULL : mapped_of(provenance);
    int done = mapped != NULL && runtime_install_operate(request->places, method, request->operation, mapped, refusal);
    char *version = done && strcmp(request->operation, "update") == 0 ? reported_version(request, application, method, mapped)
                                                                        : NULL;
    cJSON *answer = done ? answer_of(request->operation, version, version == NULL || record_version(request, version)) : NULL;
    free(version);
    cJSON_Delete(mapped);
    cJSON_Delete(registration);
    return answer;
}

static cJSON *application_for(const cJSON *argument, cJSON **refusal) {
    application_operation request = {cJSON_GetObjectItemCaseSensitive(argument, "places"),
                                     runtime_install_text(argument, "loader"), runtime_install_text(argument, "operation")};
    int usable = runtime_install_place(argument, "state") != NULL && runtime_install_place(argument, "cache") != NULL &&
                 request.loader != NULL && request.loader[0] != 0 && request.operation != NULL &&
                 (strcmp(request.operation, "update") == 0 || strcmp(request.operation, "remove") == 0);
    if (!usable) {
        *refusal = runtime_refusal("the argument carries no usable places, loader or operation", APPLICATION_FIX);
        return NULL;
    }
    return operate_through_record(&request, refusal);
}

RUNTIME_EXPORT int32_t runtime_install_application(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, application_for);
}
