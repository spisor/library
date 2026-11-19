#ifndef RUNTIME_TEST_REGISTRY_CALL_H
#define RUNTIME_TEST_REGISTRY_CALL_H

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "check.h"
#include "spisor/json_value.h"
#include "runtime/registry.h"
#include "spisor/status.h"
#include "spisor/value.h"
#include "vector_reader.h"

/**
 * @file
 * Calls the registry definition with a request a test writes as JSON, {places: {state, cache}, id, registration}, the
 * registration in the shape the record stores, and answers what the definition answered as that same JSON, so a test
 * reads a registration the way the file holds it. Test code only: the JSON is the test's data, never the boundary.
 */

typedef enum registry_operation {
    REGISTRY_REGISTER,
    REGISTRY_GET,
    REGISTRY_LIST,
    REGISTRY_FORGET
} registry_operation;

/** What a call answered: its status, the answer as JSON (an empty object for NONE), and the refusal's texts. */
typedef struct registry_result {
    spisor_status status;
    cJSON *tree;
    char detail[1024];
    char fix[1024];
} registry_result;

/** The parts of a registration read from a test's JSON, every value tree released by registry_release_parts. */
typedef struct registry_parts {
    runtime_registration registration;
    runtime_registry_provenance provenance;
    runtime_registry_start start;
    spisor_value_tree application;
    spisor_value_tree options;
} registry_parts;

static inline const char *registry_member_text(const cJSON *object, const char *name) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
}

static inline const spisor_value *registry_member_value(const cJSON *object, const char *name,
                                                        spisor_value_tree *tree) {
    tree->size = (uint32_t)sizeof *tree;
    if (cJSON_GetObjectItemCaseSensitive(object, name) == NULL) {
        return NULL;
    }
    CHECK(spisor_vector_value(object, name, tree));
    return &tree->root;
}

static inline uint32_t registry_runtime_of(const char *runtime) {
    if (runtime == NULL) {
        return 0;
    }
    return strcmp(runtime, "javascript") == 0 ? RUNTIME_REGISTRY_START_JAVASCRIPT
           : strcmp(runtime, "jvm") == 0      ? RUNTIME_REGISTRY_START_JVM
           : strcmp(runtime, "native") == 0   ? RUNTIME_REGISTRY_START_NATIVE
                                              : 99;
}

/** Reads a registration from a test's JSON; a provenance or start that is not an object is left out. */
static inline const runtime_registration *registry_parts_of(const cJSON *json, registry_parts *parts) {
    const cJSON *provenance = cJSON_GetObjectItemCaseSensitive(json, "provenance");
    const cJSON *start = cJSON_GetObjectItemCaseSensitive(json, "start");
    memset(parts, 0, sizeof *parts);
    if (json == NULL) {
        return NULL;
    }
    parts->registration.size = (uint32_t)sizeof parts->registration;
    parts->registration.id = registry_member_text(json, "id");
    parts->registration.version = registry_member_text(json, "version");
    parts->registration.home = registry_member_text(json, "home");
    parts->registration.application = registry_member_value(json, "application", &parts->application);
    parts->registration.requires_command = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "requiresCommand"));
    parts->options.size = (uint32_t)sizeof parts->options;
    if (cJSON_IsObject(provenance)) {
        parts->provenance.size = (uint32_t)sizeof parts->provenance;
        parts->provenance.method = registry_member_text(provenance, "method");
        parts->provenance.version = registry_member_text(provenance, "version");
        parts->provenance.location = registry_member_text(provenance, "location");
        parts->provenance.os = registry_member_text(provenance, "os");
        parts->provenance.architecture = registry_member_text(provenance, "architecture");
        parts->provenance.installed_at = registry_member_text(provenance, "installedAt");
        parts->provenance.package = registry_member_text(provenance, "package");
        parts->provenance.options = registry_member_value(provenance, "options", &parts->options);
        parts->registration.provenance = &parts->provenance;
    }
    if (cJSON_IsObject(start)) {
        parts->start.size = (uint32_t)sizeof parts->start;
        parts->start.runtime = registry_runtime_of(registry_member_text(start, "runtime"));
        parts->start.entry = registry_member_text(start, "entry");
        parts->registration.start = &parts->start;
    }
    return &parts->registration;
}

static inline void registry_release_parts(registry_parts *parts) {
    spisor_value_release(&parts->application);
    spisor_value_release(&parts->options);
}

/** A value tree as JSON, NULL for none. */
static inline cJSON *registry_json_of_value(const spisor_value *value) {
    cJSON *json = NULL;
    CHECK(spisor_json_of_value(value, &json) == SPISOR_OK);
    return json;
}

static inline void registry_add_text(cJSON *object, const char *name, const char *text) {
    if (text != NULL) {
        cJSON_AddStringToObject(object, name, text);
    }
}

/** A registration as the record stores it, so a test reads it as it reads the file. */
static inline cJSON *registry_json_of(const runtime_registration *registration) {
    static const char *const runtimes[] = {NULL, "javascript", "jvm", "native"};
    cJSON *json = cJSON_CreateObject();
    CHECK(registration->size == sizeof *registration);
    registry_add_text(json, "id", registration->id);
    cJSON_AddStringToObject(json, "kind", "host");
    registry_add_text(json, "version", registration->version);
    registry_add_text(json, "home", registration->home);
    if (registration->application != NULL) {
        cJSON_AddItemToObject(json, "application", registry_json_of_value(registration->application));
    }
    if (registration->provenance != NULL) {
        const runtime_registry_provenance *provenance = registration->provenance;
        cJSON *object = cJSON_AddObjectToObject(json, "provenance");
        registry_add_text(object, "method", provenance->method);
        registry_add_text(object, "package", provenance->package);
        registry_add_text(object, "version", provenance->version);
        registry_add_text(object, "location", provenance->location);
        registry_add_text(object, "os", provenance->os);
        registry_add_text(object, "architecture", provenance->architecture);
        registry_add_text(object, "installedAt", provenance->installed_at);
        if (provenance->options != NULL) {
            cJSON_AddItemToObject(object, "options", registry_json_of_value(provenance->options));
        }
    }
    cJSON_AddBoolToObject(json, "requiresCommand", registration->requires_command != 0);
    if (registration->start != NULL) {
        cJSON *object = cJSON_AddObjectToObject(json, "start");
        registry_add_text(object, "runtime", registration->start->runtime <= 3 ? runtimes[registration->start->runtime]
                                                                               : NULL);
        registry_add_text(object, "entry", registration->start->entry);
    }
    return json;
}

static inline void registry_keep_refusal(registry_result *result, const spisor_refusal *refusal) {
    snprintf(result->detail, sizeof result->detail, "%s", refusal->detail == NULL ? "" : refusal->detail);
    snprintf(result->fix, sizeof result->fix, "%s", refusal->fix == NULL ? "" : refusal->fix);
}

static inline void registry_answer_registration(registry_result *result,
                                                const runtime_registry_registration_answer *answer) {
    CHECK(result->status != SPISOR_OK || answer->registration != NULL);
    if (result->status == SPISOR_OK && answer->registration != NULL) {
        cJSON_AddItemToObject(result->tree, "registration", registry_json_of(answer->registration));
    }
}

static inline void registry_answer_listing(registry_result *result, const runtime_registry_listing *listing) {
    cJSON *hosts = cJSON_AddArrayToObject(result->tree, "hosts");
    cJSON *unreadable = cJSON_AddArrayToObject(result->tree, "unreadable");
    for (size_t index = 0; index < listing->host_count; index++) {
        cJSON *host = cJSON_CreateObject();
        cJSON_AddItemToObject(host, "registration", registry_json_of(listing->hosts[index].registration));
        cJSON_AddBoolToObject(host, "orphan", listing->hosts[index].orphan != 0);
        cJSON_AddItemToArray(hosts, host);
    }
    for (size_t index = 0; index < listing->unreadable_count; index++) {
        cJSON_AddItemToArray(unreadable, cJSON_CreateString(listing->unreadable[index]));
    }
}

/** Calls one registry operation with the request a test wrote, deleting the request. */
static inline registry_result registry_call(registry_operation operation, cJSON *request) {
    const runtime_registry_definition *registry = runtime_registry_definition_of();
    const cJSON *places = cJSON_GetObjectItemCaseSensitive(request, "places");
    const char *state = registry_member_text(places, "state");
    const char *id = registry_member_text(request, "id");
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    registry_result result;
    memset(&result, 0, sizeof result);
    result.tree = cJSON_CreateObject();
    if (operation == REGISTRY_REGISTER) {
        registry_parts parts;
        runtime_registry_register_argument argument = SPISOR_INIT(runtime_registry_register_argument);
        runtime_registry_registration_answer answer = SPISOR_INIT(runtime_registry_registration_answer);
        argument.state = state;
        argument.registration = registry_parts_of(cJSON_GetObjectItemCaseSensitive(request, "registration"), &parts);
        result.status = registry->register_host(registry->context, &argument, &answer, &refusal);
        registry_answer_registration(&result, &answer);
        registry->release_answer(registry->context, &answer);
        registry_release_parts(&parts);
    } else if (operation == REGISTRY_GET) {
        runtime_registry_host_argument argument = SPISOR_INIT(runtime_registry_host_argument);
        runtime_registry_registration_answer answer = SPISOR_INIT(runtime_registry_registration_answer);
        argument.state = state;
        argument.id = id;
        result.status = registry->get(registry->context, &argument, &answer, &refusal);
        registry_answer_registration(&result, &answer);
        registry->release_answer(registry->context, &answer);
    } else if (operation == REGISTRY_LIST) {
        runtime_registry_list_argument argument = SPISOR_INIT(runtime_registry_list_argument);
        runtime_registry_listing answer = SPISOR_INIT(runtime_registry_listing);
        argument.state = state;
        result.status = registry->list(registry->context, &argument, &answer, &refusal);
        registry_answer_listing(&result, &answer);
        registry->release_listing(registry->context, &answer);
    } else {
        runtime_registry_forget_argument argument = SPISOR_INIT(runtime_registry_forget_argument);
        runtime_registry_forget_answer answer = SPISOR_INIT(runtime_registry_forget_answer);
        argument.state = state;
        argument.cache = registry_member_text(places, "cache");
        argument.id = id;
        result.status = registry->forget(registry->context, &argument, &answer, &refusal);
        cJSON_AddBoolToObject(result.tree, "forgotten", answer.forgotten != 0);
    }
    registry_keep_refusal(&result, &refusal);
    registry->release_refusal(registry->context, &refusal);
    cJSON_Delete(request);
    return result;
}

/** Whether a call answered OK, saying why on stderr when it did not, the answer released either way. */
static inline int registry_succeeds(registry_result result, const char *label) {
    int succeeded = result.status == SPISOR_OK;
    if (!succeeded) {
        fprintf(stderr, "%s: status %d %s\n", label, (int)result.status, result.detail);
    }
    cJSON_Delete(result.tree);
    return succeeded;
}

/** Whether a call answered `expected` with a refusal whose detail or fix names `text`, any text for NULL. */
static inline int registry_refused_as(registry_result result, spisor_status expected, const char *text) {
    int matched = result.status == expected && result.detail[0] != '\0' && result.fix[0] != '\0' &&
                  (text == NULL || strstr(result.detail, text) != NULL || strstr(result.fix, text) != NULL);
    if (!matched) {
        fprintf(stderr, "expected status %d refusing with \"%s\", got status %d refusing with \"%s\"\n", (int)expected,
                text == NULL ? "(any)" : text, (int)result.status, result.detail);
    }
    cJSON_Delete(result.tree);
    return matched;
}

#endif
