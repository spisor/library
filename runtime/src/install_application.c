#include "install_ecosystem.h"
#include "allocation.h"

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

typedef enum version_record { VERSION_RECORDED, VERSION_UNRECORDED, VERSION_OUT_OF_MEMORY } version_record;

/** The loader's registration, or NULL: *absent set when none is registered, else *refusal set, or NULL for memory. */
static cJSON *registration_of(const application_operation *request, int *absent, cJSON **refusal) {
    cJSON *registration = NULL;
    int read = runtime_registry_get(runtime_install_text(request->places, "state"), request->loader, &registration,
                                    refusal);
    *absent = read && registration == NULL;
    return registration;
}

static cJSON *refused(const char *const *parts, size_t count, const char *fix_verb, const char *application) {
    const char *fix_parts[3] = {fix_verb, application, " with whatever installed it"};
    char *fix = runtime_cache_text(fix_parts, 3);
    cJSON *refusal = fix == NULL ? NULL : runtime_cache_refusal(parts, count, fix);
    spisor_memory_release(fix);
    return refusal;
}

/**
 * Whether the application's route is refused, *refusal then set, or NULL when memory ran out building it.
 *
 * @implNote A route that is path or missing was never an install method's to change, so it says where the application
 * came from rather than guessing.
 */
static int route_refused(const cJSON *application, const char *operation, cJSON **refusal) {
    const char *id = runtime_install_text(application, "id");
    const cJSON *provenance = cJSON_GetObjectItemCaseSensitive(application, "provenance");
    const char *method = runtime_install_text(provenance, "method");
    const char *verb = strcmp(operation, "update") == 0 ? "update " : "remove ";
    if (method == NULL) {
        const char *parts[3] = {"how ", id, " was installed is not recorded"};
        *refusal = refused(parts, 3, verb, id);
        return 1;
    }
    if (strcmp(method, "path") == 0) {
        const char *location = runtime_install_text(provenance, "location");
        const char *parts[4] = {id, " was found on this machine at ",
                                location == NULL ? "an unrecorded place" : location,
                                ", not installed through an install method"};
        *refusal = refused(parts, 4, verb, id);
        return 1;
    }
    if (runtime_install_text(provenance, "package") == NULL) {
        const char *parts[5] = {"the record of ", id, " names method ", method, " and no package"};
        *refusal = refused(parts, 5, verb, id);
        return 1;
    }
    return 0;
}

static cJSON *recorded_application(const application_operation *request, cJSON **registration, cJSON **refusal) {
    int absent = 0;
    *registration = registration_of(request, &absent, refusal);
    if (*registration == NULL) {
        if (absent) {
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
    return route_refused(application, request->operation, refusal) ? NULL : application;
}

static cJSON *mapped_of(const cJSON *provenance) {
    cJSON *mapped = cJSON_CreateObject();
    if (mapped == NULL ||
        !runtime_install_copy_into(mapped, "name", cJSON_GetObjectItemCaseSensitive(provenance, "package")) ||
        !runtime_install_copy_into(mapped, "options", cJSON_GetObjectItemCaseSensitive(provenance, "options"))) {
        cJSON_Delete(mapped);
        return NULL;
    }
    return mapped;
}

/** The version the method reports for the package now, or NULL when it reports none or, *failed set, memory ran out. */
static char *reported_version(const application_operation *request, const cJSON *application, const char *method,
                              const cJSON *mapped, int *failed) {
    const runtime_install_method *found = runtime_install_method_named(method);
    const char *id = runtime_install_text(application, "id");
    cJSON *detection = found == NULL ? NULL : runtime_install_detection_of(request->places, id, found, mapped);
    const cJSON *answer = cJSON_GetObjectItemCaseSensitive(detection, "answer");
    const char *version = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answer, "installed"))
                              ? runtime_install_text(answer, "version")
                              : NULL;
    char *copy = version == NULL ? NULL : spisor_memory_allocate(strlen(version) + 1);
    if (copy != NULL) {
        strcpy(copy, version);
    }
    *failed = (found != NULL && detection == NULL) || (version != NULL && copy == NULL);
    cJSON_Delete(detection);
    return copy;
}

/**
 * @implNote registry.register replaces the registration's application whole, so the registration is
 * read again right before the write and only application.provenance.version differs from it; a change
 * made since the operation began is kept.
 */
static version_record record_version(const application_operation *request, const char *version) {
    cJSON *refusal = NULL;
    int absent = 0;
    cJSON *registration = registration_of(request, &absent, &refusal);
    cJSON *application = cJSON_GetObjectItemCaseSensitive(registration, "application");
    cJSON *provenance = cJSON_GetObjectItemCaseSensitive(application, "provenance");
    if (provenance == NULL) {
        int memory_ran_out = registration == NULL && !absent && refusal == NULL;
        cJSON_Delete(refusal);
        cJSON_Delete(registration);
        return memory_ran_out ? VERSION_OUT_OF_MEMORY : VERSION_UNRECORDED;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(provenance, "version");
    if (cJSON_AddStringToObject(provenance, "version", version) == NULL) {
        cJSON_Delete(registration);
        return VERSION_OUT_OF_MEMORY;
    }
    cJSON *registered = runtime_registry_register(runtime_install_text(request->places, "state"), registration,
                                                  &refusal);
    cJSON_Delete(registration);
    int memory_ran_out = registered == NULL && refusal == NULL;
    cJSON_Delete(refusal);
    cJSON_Delete(registered);
    if (memory_ran_out) {
        return VERSION_OUT_OF_MEMORY;
    }
    return registered != NULL ? VERSION_RECORDED : VERSION_UNRECORDED;
}

/**
 * @implNote An update the method carried out is done whether or not its version could be recorded; the
 * unrecorded version is noted so the caller can say so.
 */
static cJSON *answer_of(const char *operation, const char *version, int recorded) {
    cJSON *answer = cJSON_CreateObject();
    cJSON *unrecorded =
        recorded ? NULL
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
    int failed = 0;
    char *version = done && strcmp(request->operation, "update") == 0
                        ? reported_version(request, application, method, mapped, &failed)
                        : NULL;
    version_record record = version == NULL ? VERSION_RECORDED : record_version(request, version);
    cJSON *answer = done && !failed && record != VERSION_OUT_OF_MEMORY
                        ? answer_of(request->operation, version, record == VERSION_RECORDED)
                        : NULL;
    spisor_memory_release(version);
    cJSON_Delete(mapped);
    cJSON_Delete(registration);
    return answer;
}

cJSON *runtime_install_application_operation(const cJSON *argument, cJSON **refusal) {
    application_operation request = {cJSON_GetObjectItemCaseSensitive(argument, "places"),
                                     runtime_install_text(argument, "loader"),
                                     runtime_install_text(argument, "operation")};
    int usable = runtime_install_place(argument, "state") != NULL && runtime_install_place(argument, "cache") != NULL &&
                 request.loader != NULL && request.loader[0] != 0 && request.operation != NULL &&
                 (strcmp(request.operation, "update") == 0 || strcmp(request.operation, "remove") == 0);
    if (!usable) {
        *refusal = runtime_refusal("the argument carries no usable places, loader or operation", APPLICATION_FIX);
        return NULL;
    }
    return operate_through_record(&request, refusal);
}
