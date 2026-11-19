#ifndef RUNTIME_TEST_LOADER_CALL_H
#define RUNTIME_TEST_LOADER_CALL_H

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "cache_call.h"
#include "catalog_call.h"
#include "catalog_service_call.h"
#include "check.h"
#include "definition_support.h"
#include "spisor/json_value.h"
#include "runtime/loader.h"
#include "spisor/status.h"

/**
 * @file
 * Calls the loader definition with a request a test writes as JSON, in the shape loader's operations take inside
 * runtime, and answers what the definition answered as that operation's JSON, so a test reads an answer the way
 * runtime's own callers do. A refusal answers {message, fix} with the definition's status. Test code only: the JSON is
 * the test's data, never the boundary.
 */

typedef enum loader_operation {
    LOADER_CHECK,
    LOADER_HOME,
    LOADER_INSTALLED
} loader_operation;

static inline spisor_status loader_check_call(const runtime_loader_definition *loader, const cJSON *request,
                                              catalog_request *held, cJSON **tree, spisor_refusal *refusal) {
    runtime_loader_check_argument argument = SPISOR_INIT(runtime_loader_check_argument);
    runtime_loader_check_answer answer = SPISOR_INIT(runtime_loader_check_answer);
    spisor_status status;
    argument.standard = catalog_request_value(held, request, "standard");
    status = loader->check(loader->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        cJSON *issues = cJSON_CreateArray();
        for (size_t index = 0; index < answer.issue_count; index++) {
            cJSON *each = cJSON_CreateObject();
            cache_add_text(each, "path", answer.issues[index].path);
            cache_add_text(each, "detail", answer.issues[index].detail);
            cache_add_text(each, "fix", answer.issues[index].fix);
            CHECK(cJSON_AddItemToArray(issues, each));
        }
        *tree = cJSON_CreateObject();
        CHECK(cJSON_AddItemToObject(*tree, "issues", issues));
    }
    loader->release_check_answer(loader->context, &answer);
    return status;
}

static inline spisor_status loader_home_call(const runtime_loader_definition *loader, const cJSON *request,
                                             catalog_request *held, cJSON **tree, spisor_refusal *refusal) {
    static const char *const sources[] = {NULL, "override", "native", "xdg", "candidate", "default"};
    const cJSON *home = cJSON_GetObjectItemCaseSensitive(request, "home");
    const cJSON *candidates = cJSON_GetObjectItemCaseSensitive(home, "candidates");
    const cJSON *candidate;
    const char **texts = catalog_request_block(held, (size_t)cJSON_GetArraySize(candidates), sizeof *texts);
    runtime_loader_home_argument argument = SPISOR_INIT(runtime_loader_home_argument);
    runtime_loader_home_answer answer = SPISOR_INIT(runtime_loader_home_answer);
    spisor_status status;
    cJSON_ArrayForEach(candidate, candidates) {
        texts[argument.candidate_count++] = cJSON_GetStringValue(candidate);
    }
    argument.env_override = cache_member_text(home, "envOverride");
    argument.native_env = cache_member_text(home, "nativeEnv");
    argument.xdg_subdir = cache_member_text(home, "xdgSubdir");
    argument.candidates = texts;
    argument.environment = runtime_variables_of_json(cJSON_GetObjectItemCaseSensitive(request, "environment"),
                                                     &argument.environment_count);
    CHECK(held->block_count < 8);
    held->blocks[held->block_count++] = (void *)argument.environment;
    status = loader->home(loader->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        *tree = cJSON_CreateObject();
        cache_add_text(*tree, "home", answer.home);
        cache_add_text(*tree, "source", answer.source <= 5 ? sources[answer.source] : NULL);
    }
    loader->release_home_answer(loader->context, &answer);
    return status;
}

static inline spisor_status loader_installed_call(const runtime_loader_definition *loader, const cJSON *request,
                                                  catalog_request *held, cJSON **tree, spisor_refusal *refusal) {
    runtime_loader_installed_argument argument = SPISOR_INIT(runtime_loader_installed_argument);
    runtime_loader_installed_answer answer = SPISOR_INIT(runtime_loader_installed_answer);
    runtime_item_version installed = {cache_member_text(request, "id"), cache_member_text(request, "version")};
    spisor_status status;
    (void)held;
    argument.cache = cache_member_text(cJSON_GetObjectItemCaseSensitive(request, "places"), "cache");
    argument.loader = &installed;
    status = loader->installed(loader->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        cJSON *standard = NULL;
        CHECK(spisor_json_of_value(answer.standard, &standard) == SPISOR_OK);
        *tree = cJSON_CreateObject();
        CHECK(cJSON_AddItemToObject(*tree, "standard", standard));
    }
    loader->release_installed_answer(loader->context, &answer);
    return status;
}

typedef spisor_status (*loader_caller)(const runtime_loader_definition *loader, const cJSON *request,
                                       catalog_request *held, cJSON **tree, spisor_refusal *refusal);

/** Calls `operation` with `request`, which it frees, answering the definition's status and the answer as JSON. */
static inline catalog_answer loader_call(loader_operation operation, cJSON *request) {
    static const loader_caller callers[] = {loader_check_call, loader_home_call, loader_installed_call};
    const runtime_loader_definition *loader = runtime_loader_definition_of();
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    catalog_request held;
    catalog_answer result = {0, NULL};
    spisor_status status;
    memset(&held, 0, sizeof held);
    status = callers[operation](loader, request, &held, &result.tree, &refusal);
    if (status != SPISOR_OK) {
        result.tree = cache_refusal_tree(&refusal);
    }
    loader->release_refusal(loader->context, &refusal);
    catalog_request_release(&held);
    cJSON_Delete(request);
    result.status = (int32_t)status;
    return result;
}

#endif
