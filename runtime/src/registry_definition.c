#include "runtime/registry.h"

#include <string.h>

#include "answer_storage.h"
#include "definition_support.h"
#include "registry_definition.h"
#include "json_service.h"
#include "json_value.h"
#include "paths.h"
#include "registry.h"

#define PROVENANCE_SHAPE RUNTIME_SHAPE_THROUGH(runtime_registry_provenance, options)
#define START_SHAPE RUNTIME_SHAPE_THROUGH(runtime_registry_start, entry)
#define REGISTRATION_SHAPE RUNTIME_SHAPE_THROUGH(runtime_registration, start)
#define REGISTER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_registry_register_argument, registration)
#define HOST_SHAPE RUNTIME_SHAPE_THROUGH(runtime_registry_host_argument, id)
#define LIST_SHAPE RUNTIME_SHAPE_THROUGH(runtime_registry_list_argument, state)
#define FORGET_SHAPE RUNTIME_SHAPE_THROUGH(runtime_registry_forget_argument, id)
#define ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_registry_registration_answer, storage)
#define LISTING_SHAPE RUNTIME_SHAPE_THROUGH(runtime_registry_listing, storage)
#define FORGET_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_registry_forget_answer, forgotten)

#define REGISTRATION_FIX                                                                                         \
    "send a registration with a host id, a non-empty version, an absolute home, an application record or NULL, " \
    "a provenance with method, version, an absolute location, os, architecture and an ISO 8601 UTC installed_at, " \
    "and a start, when given, with a known runtime and a relative entry inside the bundle"

#define UNUSABLE_PARTS "the registration's provenance, start or a value is unusable"

static const char *const START_RUNTIMES[] = {NULL, "javascript", "jvm", "native"};

static spisor_status refuse_id(spisor_refusal *refusal) {
    return runtime_refuse_as(refusal, SPISOR_MALFORMED, "the argument carries no usable host id",
                             "send id as the host id, one portable path segment with no surrounding whitespace");
}

static int add_text(cJSON *object, const char *name, const char *text) {
    return text == NULL || cJSON_AddStringToObject(object, name, text) != NULL;
}

/** Adds a value tree under name, leaving a NULL value out; MALFORMED for a value that is not well-formed. */
static spisor_status add_value(cJSON *object, const char *name, const spisor_value *value) {
    cJSON *json = NULL;
    spisor_status status = value == NULL ? SPISOR_OK : spisor_json_of_value(value, &json);
    if (status == SPISOR_OK && json != NULL && !cJSON_AddItemToObject(object, name, json)) {
        cJSON_Delete(json);
        status = SPISOR_OUT_OF_MEMORY;
    }
    return status;
}

static spisor_status add_provenance(cJSON *registration, const runtime_registry_provenance *provenance) {
    cJSON *json;
    if (provenance == NULL || !runtime_has_shape(provenance->size, PROVENANCE_SHAPE)) {
        return SPISOR_MALFORMED;
    }
    json = cJSON_AddObjectToObject(registration, "provenance");
    if (json == NULL || !add_text(json, "method", provenance->method) ||
        !add_text(json, "package", provenance->package) || !add_text(json, "version", provenance->version) ||
        !add_text(json, "location", provenance->location) || !add_text(json, "os", provenance->os) ||
        !add_text(json, "architecture", provenance->architecture) ||
        !add_text(json, "installedAt", provenance->installed_at)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    return add_value(json, "options", provenance->options);
}

static spisor_status add_start(cJSON *registration, const runtime_registry_start *start) {
    cJSON *json;
    int known;
    if (start == NULL) {
        return SPISOR_OK;
    }
    if (!runtime_has_shape(start->size, START_SHAPE)) {
        return SPISOR_MALFORMED;
    }
    known = start->runtime >= RUNTIME_REGISTRY_START_JAVASCRIPT && start->runtime <= RUNTIME_REGISTRY_START_NATIVE;
    json = cJSON_AddObjectToObject(registration, "start");
    if (json == NULL || !add_text(json, "runtime", known ? START_RUNTIMES[start->runtime] : NULL) ||
        !add_text(json, "entry", start->entry)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    return SPISOR_OK;
}

/** The registration as the record runtime_registry_register takes, NULL unless the answer is OK. */
static spisor_status registration_json(const runtime_registration *registration, cJSON **json) {
    spisor_status status = SPISOR_OUT_OF_MEMORY;
    *json = cJSON_CreateObject();
    if (*json != NULL && add_text(*json, "id", registration->id) && add_text(*json, "kind", "host") &&
        add_text(*json, "version", registration->version) && add_text(*json, "home", registration->home) &&
        cJSON_AddBoolToObject(*json, "requiresCommand", registration->requires_command != 0) != NULL) {
        status = add_value(*json, "application", registration->application);
    }
    if (status == SPISOR_OK) {
        status = add_provenance(*json, registration->provenance);
    }
    if (status == SPISOR_OK) {
        status = add_start(*json, registration->start);
    }
    if (status != SPISOR_OK) {
        cJSON_Delete(*json);
        *json = NULL;
    }
    return status;
}

static const char *text_of(const cJSON *object, const char *name) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static uint32_t start_runtime_of(const char *runtime) {
    for (uint32_t kind = RUNTIME_REGISTRY_START_JAVASCRIPT; runtime != NULL && kind <= RUNTIME_REGISTRY_START_NATIVE;
         kind++) {
        if (strcmp(runtime, START_RUNTIMES[kind]) == 0) {
            return kind;
        }
    }
    return 0;
}

static spisor_status fill_start(void **storage, const cJSON *json, const runtime_registry_start **out) {
    runtime_registry_start *start;
    *out = NULL;
    if (!cJSON_IsObject(json)) {
        return SPISOR_OK;
    }
    start = runtime_storage_take(storage, sizeof *start);
    if (start == NULL || !runtime_storage_copy(storage, text_of(json, "entry"), &start->entry)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    start->size = (uint32_t)sizeof *start;
    start->runtime = start_runtime_of(text_of(json, "runtime"));
    *out = start;
    return SPISOR_OK;
}

/** A recorded registration in the storage, MALFORMED for a record holding what no value tree carries. */
static spisor_status fill_registration(void **storage, const cJSON *json, const runtime_registration **out) {
    runtime_registration *registration = runtime_storage_take(storage, sizeof *registration);
    spisor_status status = SPISOR_OUT_OF_MEMORY;
    if (registration != NULL && runtime_storage_copy(storage, text_of(json, "id"), &registration->id) &&
        runtime_storage_copy(storage, text_of(json, "version"), &registration->version) &&
        runtime_storage_copy(storage, text_of(json, "home"), &registration->home)) {
        registration->size = (uint32_t)sizeof *registration;
        registration->requires_command = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "requiresCommand"));
        status = runtime_storage_value(storage, cJSON_GetObjectItemCaseSensitive(json, "application"),
                                       &registration->application);
    }
    if (status == SPISOR_OK) {
        status = runtime_storage_provenance(storage, cJSON_GetObjectItemCaseSensitive(json, "provenance"),
                                 &registration->provenance);
    }
    if (status == SPISOR_OK) {
        status = fill_start(storage, cJSON_GetObjectItemCaseSensitive(json, "start"), &registration->start);
    }
    *out = status == SPISOR_OK ? registration : NULL;
    return status;
}

/** The status a fill answered, REFUSED for a record holding what no value tree carries. */
static spisor_status refuse_unfilled(spisor_status status, spisor_refusal *refusal) {
    if (status != SPISOR_MALFORMED) {
        return status;
    }
    return runtime_refuse_as(refusal, SPISOR_REFUSED, "a host's record holds a value no value tree carries",
                             "repair the host's record under the state place's hosts folder, or register it again");
}

spisor_status runtime_registration_record_json(const runtime_registration *registration, cJSON **json,
                                               spisor_refusal *refusal) {
    const char *defect;
    spisor_status status;
    *json = NULL;
    if (registration == NULL || !runtime_has_shape(registration->size, REGISTRATION_SHAPE)) {
        return runtime_refuse_as(refusal, SPISOR_MALFORMED, "the registration is absent or short of its first shape",
                                 REGISTRATION_FIX);
    }
    status = registration_json(registration, json);
    defect = status == SPISOR_OK ? runtime_registration_defect(*json) : NULL;
    if (status == SPISOR_MALFORMED || defect != NULL) {
        cJSON_Delete(*json);
        *json = NULL;
        return runtime_refuse_as(refusal, SPISOR_MALFORMED, defect != NULL ? defect : UNUSABLE_PARTS, REGISTRATION_FIX);
    }
    return status;
}

/**
 * Fills the answer from `record` and only then records it, so memory running out writes nothing. The record keeps
 * exactly the registration's own fields, so the answer is what it stores.
 */
static spisor_status register_answered_first(const char *state, const cJSON *record,
                                             runtime_registry_registration_answer *answer, spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    cJSON *registered;
    spisor_status status = fill_registration(&answer->storage, record, &answer->registration);
    if (status != SPISOR_OK) {
        runtime_release_stored_answer(answer, ANSWER_SHAPE);
        return status;
    }
    registered = runtime_registry_register(state, record, &operation_refusal);
    if (registered == NULL) {
        runtime_release_stored_answer(answer, ANSWER_SHAPE);
        return runtime_refuse_json(refusal, operation_refusal);
    }
    cJSON_Delete(registered);
    return SPISOR_OK;
}

static spisor_status register_host(void *context, const runtime_registry_register_argument *argument,
                                   runtime_registry_registration_answer *answer, spisor_refusal *refusal) {
    cJSON *record = NULL;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, REGISTER_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "registry's register_host");
    }
    if (!runtime_is_usable_home(argument->state)) {
        return runtime_refuse_place(refusal, "state");
    }
    status = runtime_registration_record_json(argument->registration, &record, refusal);
    if (status == SPISOR_OK) {
        status = register_answered_first(argument->state, record, answer, refusal);
    }
    cJSON_Delete(record);
    return status;
}

static spisor_status get(void *context, const runtime_registry_host_argument *argument,
                         runtime_registry_registration_answer *answer, spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    cJSON *registration = NULL;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, HOST_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "registry's get");
    }
    if (!runtime_is_usable_home(argument->state)) {
        return runtime_refuse_place(refusal, "state");
    }
    if (!runtime_registry_is_host_id(argument->id)) {
        return refuse_id(refusal);
    }
    if (!runtime_registry_get(argument->state, argument->id, &registration, &operation_refusal)) {
        return runtime_refuse_json(refusal, operation_refusal);
    }
    if (registration == NULL) {
        return SPISOR_NONE;
    }
    status = fill_registration(&answer->storage, registration, &answer->registration);
    cJSON_Delete(registration);
    if (status != SPISOR_OK) {
        runtime_release_stored_answer(answer, ANSWER_SHAPE);
    }
    return refuse_unfilled(status, refusal);
}

static spisor_status fill_hosts(const cJSON *hosts, runtime_registry_listing *answer) {
    runtime_registry_host *filled;
    const cJSON *host;
    size_t index = 0;
    spisor_status status = SPISOR_OK;
    if (!runtime_storage_take_array(&answer->storage, (size_t)cJSON_GetArraySize(hosts), sizeof *filled,
                                    (void **)&filled)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    cJSON_ArrayForEach(host, hosts) {
        if (status == SPISOR_OK) {
            filled[index].orphan = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(host, "orphan")) ? 1 : 0;
            status = fill_registration(&answer->storage, cJSON_GetObjectItemCaseSensitive(host, "registration"),
                                       &filled[index].registration);
            index++;
        }
    }
    answer->hosts = filled;
    answer->host_count = index;
    return status;
}

static spisor_status fill_listing(const cJSON *listed, runtime_registry_listing *answer, spisor_refusal *refusal) {
    spisor_status status = fill_hosts(cJSON_GetObjectItemCaseSensitive(listed, "hosts"), answer);
    if (status == SPISOR_OK &&
        !runtime_storage_texts(&answer->storage, cJSON_GetObjectItemCaseSensitive(listed, "unreadable"),
                               &answer->unreadable, &answer->unreadable_count)) {
        status = SPISOR_OUT_OF_MEMORY;
    }
    if (status != SPISOR_OK) {
        runtime_release_stored_answer(answer, LISTING_SHAPE);
    }
    return refuse_unfilled(status, refusal);
}

static spisor_status list(void *context, const runtime_registry_list_argument *argument,
                          runtime_registry_listing *answer, spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    cJSON *listed;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, LIST_SHAPE) ||
        !RUNTIME_IS_EMPTY_COUNTED_ANSWER(answer, LISTING_SHAPE, answer->host_count | answer->unreadable_count)) {
        return runtime_refuse_shape(refusal, "registry's list");
    }
    if (!runtime_is_usable_home(argument->state)) {
        return runtime_refuse_place(refusal, "state");
    }
    listed = runtime_registry_list(argument->state, &operation_refusal);
    status = listed == NULL ? runtime_refuse_json(refusal, operation_refusal) : fill_listing(listed, answer, refusal);
    cJSON_Delete(listed);
    return status;
}

static spisor_status forget(void *context, const runtime_registry_forget_argument *argument,
                            runtime_registry_forget_answer *answer, spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    int forgotten;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, FORGET_SHAPE) || answer == NULL ||
        !runtime_has_shape(answer->size, FORGET_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "registry's forget");
    }
    if (!runtime_is_usable_home(argument->state) || !runtime_is_usable_home(argument->cache)) {
        return runtime_refuse_place(refusal, runtime_is_usable_home(argument->state) ? "cache" : "state");
    }
    if (!runtime_registry_is_host_id(argument->id)) {
        return refuse_id(refusal);
    }
    forgotten = runtime_registry_forget(argument->state, argument->cache, argument->id, &operation_refusal);
    if (forgotten < 0) {
        return runtime_refuse_json(refusal, operation_refusal);
    }
    answer->forgotten = (uint32_t)forgotten;
    return SPISOR_OK;
}

static void release_answer(void *context, runtime_registry_registration_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, ANSWER_SHAPE);
}

static void release_listing(void *context, runtime_registry_listing *answer) {
    (void)context;
    runtime_release_stored_answer(answer, LISTING_SHAPE);
}

static const runtime_registry_definition definition = {
    sizeof(runtime_registry_definition), NULL, register_host, get, list, forget, release_answer, release_listing,
    runtime_release_refusal,
};

RUNTIME_DEFINITION_API const void *runtime_registry_definition_of(void) {
    return &definition;
}
