#ifndef RUNTIME_TEST_INSTALL_CALL_H
#define RUNTIME_TEST_INSTALL_CALL_H

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "cache_call.h"
#include "catalog_call.h"
#include "catalog_service_call.h"
#include "check.h"
#include "runtime/install.h"
#include "spisor/status.h"
#include "spisor/value.h"

/**
 * @file
 * Calls the install definition with a request a test writes as JSON, in the shape install's operations take inside
 * runtime, and answers what the definition answered as that operation's JSON. A refusal answers {message, fix} with
 * the definition's status. Test code only: the JSON is the test's data, never the boundary.
 */

typedef enum install_operation {
    INSTALL_WARNINGS,
    INSTALL_PACKAGES,
    INSTALL_PLAN,
    INSTALL_DETECT
} install_operation;

static inline const char *install_place_of(const cJSON *request, const char *name) {
    return cache_member_text(cJSON_GetObjectItemCaseSensitive(request, "places"), name);
}

/** The texts of a JSON array of strings, a non-string item read as NULL; NULL for an absent array. */
static inline const char *const *install_texts_of(catalog_request *held, const cJSON *array, size_t *count) {
    const char **texts;
    const cJSON *entry;
    *count = 0;
    if (array == NULL) {
        return NULL;
    }
    texts = catalog_request_block(held, (size_t)cJSON_GetArraySize(array), sizeof(const char *));
    cJSON_ArrayForEach(entry, array) {
        texts[(*count)++] = cJSON_GetStringValue(entry);
    }
    return texts;
}

static inline void install_add_value(cJSON *object, const char *name, const spisor_value *value) {
    if (value != NULL) {
        CHECK(cJSON_AddItemToObject(object, name, catalog_record_json(value)));
    }
}

/** A package as install's operations answer one, {name, options?}. */
static inline cJSON *install_package_json(const runtime_install_package *package) {
    cJSON *json = cJSON_CreateObject();
    cache_add_text(json, "name", package->name);
    install_add_value(json, "options", package->options);
    return json;
}

static inline spisor_status install_warnings_call(const runtime_install_definition *install, const cJSON *request,
                                                  catalog_request *held, cJSON **tree, spisor_refusal *refusal) {
    static const char *const kinds[] = {"", "named-dependency", "unbound-service"};
    runtime_install_warnings_argument argument = SPISOR_INIT(runtime_install_warnings_argument);
    runtime_install_warnings_answer answer = SPISOR_INIT(runtime_install_warnings_answer);
    spisor_status status;
    argument.manifest = catalog_request_value(held, request, "manifest");
    status = install->warnings(install->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        cJSON *warnings = cJSON_CreateArray();
        for (size_t index = 0; index < answer.warning_count; index++) {
            cJSON *each = cJSON_CreateObject();
            CHECK(answer.warnings[index].kind <= RUNTIME_INSTALL_UNBOUND_SERVICE);
            cache_add_text(each, "kind", kinds[answer.warnings[index].kind]);
            cache_add_text(each, "subject", answer.warnings[index].subject);
            cache_add_text(each, "detail", answer.warnings[index].detail);
            CHECK(cJSON_AddItemToArray(warnings, each));
        }
        *tree = cJSON_CreateObject();
        CHECK(cJSON_AddItemToObject(*tree, "warnings", warnings));
    }
    install->release_warnings_answer(install->context, &answer);
    return status;
}

static inline spisor_status install_packages_call(const runtime_install_definition *install, const cJSON *request,
                                                  catalog_request *held, cJSON **tree, spisor_refusal *refusal) {
    runtime_install_packages_argument argument = SPISOR_INIT(runtime_install_packages_argument);
    runtime_install_packages_answer answer = SPISOR_INIT(runtime_install_packages_answer);
    spisor_status status;
    argument.portable = install_place_of(request, "portable");
    argument.item = cache_member_text(request, "item");
    argument.manifest_packages = catalog_request_value(held, request, "manifestPackages");
    status = install->packages(install->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        cJSON *packages = cJSON_CreateObject();
        for (size_t index = 0; index < answer.package_count; index++) {
            CHECK(cJSON_AddItemToObject(packages, answer.packages[index].method,
                                        install_package_json(&answer.packages[index])));
        }
        *tree = cJSON_CreateObject();
        CHECK(cJSON_AddItemToObject(*tree, "packages", packages));
    }
    install->release_packages_answer(install->context, &answer);
    return status;
}

static inline cJSON *install_step_json(const runtime_install_step *step) {
    cJSON *json = cJSON_CreateObject();
    if (step->kind == RUNTIME_INSTALL_PROVIDER_STEP) {
        install_add_value(json, "install-provider", step->provider);
    } else {
        cJSON *detail = cJSON_CreateObject();
        cache_add_text(detail, "method", step->package.method);
        if (step->package.name != NULL) {
            CHECK(cJSON_AddItemToObject(detail, "package", install_package_json(&step->package)));
        }
        CHECK(cJSON_AddItemToObject(json, "install", detail));
    }
    return json;
}

static inline spisor_status install_plan_call(const runtime_install_definition *install, const cJSON *request,
                                              catalog_request *held, cJSON **tree, spisor_refusal *refusal) {
    runtime_install_plan_argument argument = SPISOR_INIT(runtime_install_plan_argument);
    runtime_install_plan_answer answer = SPISOR_INIT(runtime_install_plan_answer);
    spisor_status status;
    argument.portable = install_place_of(request, "portable");
    argument.item = cache_member_text(request, "item");
    argument.manifest_packages = catalog_request_value(held, request, "manifestPackages");
    argument.methods = install_texts_of(held, cJSON_GetObjectItemCaseSensitive(request, "methods"),
                                        &argument.method_count);
    argument.chosen = cache_member_text(request, "chosen");
    argument.installed_providers = install_texts_of(
        held, cJSON_GetObjectItemCaseSensitive(request, "installedProviders"), &argument.installed_provider_count);
    argument.catalog = catalog_request_value(held, request, "catalog");
    status = install->plan(install->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        cJSON *steps = cJSON_CreateArray();
        for (size_t index = 0; index < answer.step_count; index++) {
            CHECK(cJSON_AddItemToArray(steps, install_step_json(&answer.steps[index])));
        }
        *tree = cJSON_CreateObject();
        CHECK(cJSON_AddItemToObject(*tree, "steps", steps));
    }
    install->release_plan_answer(install->context, &answer);
    return status;
}

static inline cJSON *install_detection_json(const runtime_install_detection *detection) {
    cJSON *json = cJSON_CreateObject();
    cache_add_text(json, "method", detection->package.method);
    cache_add_text(json, "item", detection->item);
    cache_add_text(json, "package", detection->package.name);
    install_add_value(json, "options", detection->package.options);
    if (detection->refused != NULL) {
        cache_add_text(json, "refused", detection->refused);
    } else {
        cJSON *found = cJSON_CreateObject();
        CHECK(cJSON_AddBoolToObject(found, "installed", detection->installed != 0) != NULL);
        cache_add_text(found, "version", detection->version);
        CHECK(cJSON_AddItemToObject(json, "answer", found));
    }
    return json;
}

static inline spisor_status install_detect_call(const runtime_install_definition *install, const cJSON *request,
                                                catalog_request *held, cJSON **tree, spisor_refusal *refusal) {
    runtime_install_detect_argument argument = SPISOR_INIT(runtime_install_detect_argument);
    runtime_install_detect_answer answer = SPISOR_INIT(runtime_install_detect_answer);
    spisor_status status;
    argument.portable = install_place_of(request, "portable");
    argument.cache = install_place_of(request, "cache");
    argument.item = cache_member_text(request, "item");
    argument.manifest_packages = catalog_request_value(held, request, "manifestPackages");
    status = install->detect(install->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        cJSON *detections = cJSON_CreateArray();
        for (size_t index = 0; index < answer.detection_count; index++) {
            CHECK(cJSON_AddItemToArray(detections, install_detection_json(&answer.detections[index])));
        }
        *tree = cJSON_CreateObject();
        CHECK(cJSON_AddItemToObject(*tree, "detections", detections));
    }
    install->release_detect_answer(install->context, &answer);
    return status;
}

/** Calls the operation with the request, which it deletes, answering its status and its answer or refusal as JSON. */
static inline catalog_answer install_call(install_operation operation, cJSON *request) {
    typedef spisor_status (*caller)(const runtime_install_definition *, const cJSON *, catalog_request *, cJSON **,
                                    spisor_refusal *);
    static const caller callers[] = {install_warnings_call, install_packages_call, install_plan_call,
                                     install_detect_call};
    const runtime_install_definition *install = runtime_install_definition_of();
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    catalog_request held;
    catalog_answer result = {0, NULL};
    spisor_status status;
    memset(&held, 0, sizeof held);
    status = callers[operation](install, request, &held, &result.tree, &refusal);
    if (status != SPISOR_OK) {
        result.tree = cache_refusal_tree(&refusal);
    }
    install->release_refusal(install->context, &refusal);
    catalog_request_release(&held);
    cJSON_Delete(request);
    result.status = (int32_t)status;
    return result;
}

#endif
