#ifndef RUNTIME_TEST_CACHE_CALL_H
#define RUNTIME_TEST_CACHE_CALL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "catalog_call.h"
#include "check.h"
#include "runtime/cache.h"
#include "spisor/status.h"
#include "spisor/value.h"
#include "vector_reader.h"

/**
 * @file
 * Calls the cache definition with a request a test writes as JSON, in the shape cache's operations take inside runtime,
 * and answers what the definition answered as that operation's JSON, so a test reads an answer the way runtime's own
 * callers do. A refusal answers {message, fix} with the definition's status. Test code only: the JSON is the test's
 * data, never the boundary.
 */

typedef enum cache_operation {
    CACHE_INSTALL,
    CACHE_INSTALLED,
    CACHE_CHECK,
    CACHE_DAMAGED,
    CACHE_LOCATE,
    CACHE_PLATFORM,
    CACHE_SWEEP,
    CACHE_DEPLOY
} cache_operation;

static inline const char *cache_member_text(const cJSON *object, const char *name) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
}

static inline void cache_add_text(cJSON *object, const char *name, const char *text) {
    if (text != NULL) {
        CHECK(cJSON_AddStringToObject(object, name, text) != NULL);
    }
}

static inline cJSON *cache_texts_json(const char *const *texts, size_t count) {
    cJSON *array = cJSON_CreateArray();
    for (size_t index = 0; index < count; index++) {
        CHECK(cJSON_AddItemToArray(array, cJSON_CreateString(texts[index])));
    }
    return array;
}

static inline cJSON *cache_damages_json(const runtime_cache_damage *damages, size_t count) {
    cJSON *array = cJSON_CreateArray();
    for (size_t index = 0; index < count; index++) {
        cJSON *entry = cJSON_CreateObject();
        cache_add_text(entry, "id", damages[index].id);
        cache_add_text(entry, "version", damages[index].version);
        cache_add_text(entry, "detail", damages[index].detail);
        cache_add_text(entry, "fix", damages[index].fix);
        cache_add_text(entry, "file", damages[index].file);
        CHECK(cJSON_AddItemToArray(array, entry));
    }
    return array;
}

static inline cJSON *cache_versions_json(const runtime_cache_versions *versions, size_t count) {
    cJSON *object = cJSON_CreateObject();
    for (size_t index = 0; index < count; index++) {
        CHECK(cJSON_AddItemToObject(object, versions[index].id,
                                    cache_texts_json(versions[index].versions, versions[index].version_count)));
    }
    return object;
}

static inline cJSON *cache_refusal_tree(const spisor_refusal *refusal) {
    cJSON *tree = cJSON_CreateObject();
    cache_add_text(tree, "message", refusal->detail);
    cache_add_text(tree, "fix", refusal->fix);
    return tree;
}

static inline spisor_status cache_install_call(const runtime_cache_definition *cache, const cJSON *request,
                                               cJSON **tree, spisor_refusal *refusal) {
    runtime_cache_install_argument argument = SPISOR_INIT(runtime_cache_install_argument);
    runtime_cache_install_answer answer = SPISOR_INIT(runtime_cache_install_answer);
    spisor_value_tree item = SPISOR_INIT(spisor_value_tree);
    spisor_status status;
    argument.cache = cache_member_text(request, "cache");
    argument.id = cache_member_text(request, "id");
    argument.version = cache_member_text(request, "version");
    argument.platform = cache_member_text(request, "platform");
    if (cJSON_GetObjectItemCaseSensitive(request, "item") != NULL) {
        CHECK(spisor_vector_value(request, "item", &item));
        argument.item = &item.root;
    }
    status = cache->install(cache->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        *tree = cJSON_CreateObject();
        cache_add_text(*tree, "id", answer.id);
        cache_add_text(*tree, "version", answer.version);
        cache_add_text(*tree, "directory", answer.directory);
        cJSON_AddItemToObject(*tree, "fetched", cache_texts_json(answer.fetched, answer.fetched_count));
        cJSON_AddItemToObject(*tree, "cached", cache_texts_json(answer.cached, answer.cached_count));
    }
    cache->release_install_answer(cache->context, &answer);
    spisor_value_release(&item);
    return status;
}

static inline spisor_status cache_place_call(const runtime_cache_definition *cache, cache_operation operation,
                                             const cJSON *request, cJSON **tree, spisor_refusal *refusal) {
    runtime_cache_place_argument argument = SPISOR_INIT(runtime_cache_place_argument);
    spisor_status status;
    argument.cache = cache_member_text(request, "cache");
    argument.platform = cache_member_text(request, "platform");
    if (operation == CACHE_INSTALLED) {
        runtime_cache_installed_answer answer = SPISOR_INIT(runtime_cache_installed_answer);
        status = cache->installed(cache->context, &argument, &answer, refusal);
        if (status == SPISOR_OK) {
            *tree = cJSON_CreateObject();
            cJSON_AddItemToObject(*tree, "bundles", cache_versions_json(answer.bundles, answer.bundle_count));
            cJSON_AddItemToObject(*tree, "libraries", cache_versions_json(answer.libraries, answer.library_count));
        }
        cache->release_installed_answer(cache->context, &answer);
    } else {
        runtime_cache_damaged_answer answer = SPISOR_INIT(runtime_cache_damaged_answer);
        status = cache->damaged(cache->context, &argument, &answer, refusal);
        if (status == SPISOR_OK) {
            *tree = cJSON_CreateObject();
            cJSON_AddItemToObject(*tree, "damaged", cache_damages_json(answer.damaged, answer.damaged_count));
        }
        cache->release_damaged_answer(cache->context, &answer);
    }
    return status;
}

static inline spisor_status cache_check_call(const runtime_cache_definition *cache, const cJSON *request,
                                             cJSON **tree, spisor_refusal *refusal) {
    static const char *const verdicts[] = {"", "absent", "intact", "damaged"};
    runtime_cache_check_argument argument = SPISOR_INIT(runtime_cache_check_argument);
    runtime_cache_check_answer answer = SPISOR_INIT(runtime_cache_check_answer);
    spisor_status status;
    argument.directory = cache_member_text(request, "directory");
    status = cache->check(cache->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        CHECK(answer.verdict >= RUNTIME_CACHE_ABSENT && answer.verdict <= RUNTIME_CACHE_DAMAGED);
        *tree = cJSON_CreateObject();
        cache_add_text(*tree, "kind", verdicts[answer.verdict <= RUNTIME_CACHE_DAMAGED ? answer.verdict : 0]);
        cache_add_text(*tree, "detail", answer.detail);
        cache_add_text(*tree, "fix", answer.fix);
        cache_add_text(*tree, "file", answer.file);
    }
    cache->release_check_answer(cache->context, &answer);
    return status;
}

static inline cJSON *cache_located_json(const runtime_cache_located *located, size_t count) {
    cJSON *array = cJSON_CreateArray();
    for (size_t index = 0; index < count; index++) {
        cJSON *entry = cJSON_CreateObject();
        cache_add_text(entry, "id", located[index].id);
        cache_add_text(entry, "version", located[index].version);
        cache_add_text(entry, "directory", located[index].directory);
        cache_add_text(entry, "manifest", located[index].manifest);
        cache_add_text(entry, "entry", located[index].entry);
        CHECK(cJSON_AddItemToArray(array, entry));
    }
    return array;
}

static inline spisor_status cache_locate_call(const runtime_cache_definition *cache, const cJSON *request,
                                              cJSON **tree, spisor_refusal *refusal) {
    const cJSON *plugins = cJSON_GetObjectItemCaseSensitive(request, "plugins");
    runtime_cache_locate_argument argument = SPISOR_INIT(runtime_cache_locate_argument);
    runtime_cache_locate_answer answer = SPISOR_INIT(runtime_cache_locate_answer);
    runtime_item_version *listed = calloc((size_t)cJSON_GetArraySize(plugins) + 1, sizeof *listed);
    const cJSON *plugin;
    spisor_status status;
    CHECK(listed != NULL);
    cJSON_ArrayForEach(plugin, plugins) {
        listed[argument.plugin_count].id = plugin->string;
        listed[argument.plugin_count++].version = cJSON_GetStringValue(plugin);
    }
    argument.cache = cache_member_text(request, "cache");
    argument.plugins = listed;
    status = cache->locate(cache->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        *tree = cJSON_CreateObject();
        cJSON_AddItemToObject(*tree, "located", cache_located_json(answer.located, answer.located_count));
        cJSON_AddItemToObject(*tree, "problems", cache_damages_json(answer.problems, answer.problem_count));
    }
    cache->release_locate_answer(cache->context, &answer);
    free(listed);
    return status;
}

static inline spisor_status cache_platform_call(const runtime_cache_definition *cache, cJSON **tree,
                                                spisor_refusal *refusal) {
    runtime_cache_platform_answer answer = SPISOR_INIT(runtime_cache_platform_answer);
    spisor_status status = cache->platform(cache->context, &answer, refusal);
    if (status == SPISOR_OK) {
        *tree = cJSON_CreateObject();
        cache_add_text(*tree, "platform", answer.platform);
    }
    cache->release_platform_answer(cache->context, &answer);
    return status;
}

static inline spisor_status cache_sweep_call(const runtime_cache_definition *cache, const cJSON *request,
                                             cJSON **tree, spisor_refusal *refusal) {
    runtime_cache_sweep_argument argument = SPISOR_INIT(runtime_cache_sweep_argument);
    runtime_cache_sweep_answer answer = SPISOR_INIT(runtime_cache_sweep_answer);
    spisor_status status;
    argument.cache = cache_member_text(request, "cache");
    argument.state = cache_member_text(request, "state");
    status = cache->sweep(cache->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        *tree = cJSON_CreateObject();
        cJSON_AddItemToObject(*tree, "removed", cache_texts_json(answer.removed, answer.removed_count));
    }
    cache->release_sweep_answer(cache->context, &answer);
    return status;
}

static inline spisor_status cache_deploy_call(const runtime_cache_definition *cache, const cJSON *request,
                                              cJSON **tree, spisor_refusal *refusal) {
    const cJSON *identity = cJSON_GetObjectItemCaseSensitive(request, "identity");
    runtime_cache_deploy_argument argument = SPISOR_INIT(runtime_cache_deploy_argument);
    runtime_cache_deploy_answer answer = SPISOR_INIT(runtime_cache_deploy_answer);
    spisor_status status;
    argument.directory = cache_member_text(request, "directory");
    argument.plugin_directory = cache_member_text(request, "pluginDir");
    argument.id = cache_member_text(identity, "id");
    argument.name = cache_member_text(identity, "name");
    argument.replaces = cache_member_text(request, "replaces");
    status = cache->deploy(cache->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        *tree = cJSON_CreateObject();
        cache_add_text(*tree, "importName", answer.import_name);
    }
    cache->release_deploy_answer(cache->context, &answer);
    return status;
}

/** Calls the operation with the request, which it deletes, answering its status and its answer or refusal as JSON. */
static inline catalog_answer cache_call(cache_operation operation, cJSON *request) {
    const runtime_cache_definition *cache = runtime_cache_definition_of();
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    catalog_answer result = {0, NULL};
    spisor_status status;
    switch (operation) {
    case CACHE_INSTALL:
        status = cache_install_call(cache, request, &result.tree, &refusal);
        break;
    case CACHE_INSTALLED:
    case CACHE_DAMAGED:
        status = cache_place_call(cache, operation, request, &result.tree, &refusal);
        break;
    case CACHE_CHECK:
        status = cache_check_call(cache, request, &result.tree, &refusal);
        break;
    case CACHE_LOCATE:
        status = cache_locate_call(cache, request, &result.tree, &refusal);
        break;
    case CACHE_PLATFORM:
        status = cache_platform_call(cache, &result.tree, &refusal);
        break;
    case CACHE_SWEEP:
        status = cache_sweep_call(cache, request, &result.tree, &refusal);
        break;
    default:
        status = cache_deploy_call(cache, request, &result.tree, &refusal);
        break;
    }
    if (status != SPISOR_OK) {
        result.tree = cache_refusal_tree(&refusal);
    }
    cache->release_refusal(cache->context, &refusal);
    cJSON_Delete(request);
    result.status = (int32_t)status;
    return result;
}

/** Whether the answer is a refusal of `expected` whose message names `fragment`, any message for NULL. */
static inline int expect_cache_refusal(catalog_answer answer, spisor_status expected, const char *fragment,
                                       const char *label) {
    const char *message = text_at(answer.tree, "message");
    int matched = answer.status == (int32_t)expected && message != NULL && text_at(answer.tree, "fix") != NULL &&
                  (fragment == NULL || strstr(message, fragment) != NULL);
    if (!matched) {
        fprintf(stderr, "%s: expected status %d refusing with \"%s\", got status %d and \"%s\"\n", label,
                (int)expected, fragment == NULL ? "" : fragment, (int)answer.status,
                message == NULL ? "(none)" : message);
        check_failures++;
    }
    return matched;
}

/** expect_cache_refusal of a refusal REFUSED. */
static inline int expect_cache_refusal_refused(catalog_answer answer, const char *fragment, const char *label) {
    return expect_cache_refusal(answer, SPISOR_REFUSED, fragment, label);
}

/** expect_cache_refusal of a refusal MALFORMED. */
static inline int expect_cache_refusal_malformed(catalog_answer answer, const char *fragment, const char *label) {
    return expect_cache_refusal(answer, SPISOR_MALFORMED, fragment, label);
}

/** expect_cache_refusal_malformed on an answer no caller keeps, releasing its tree. */
static inline int expect_malformed_released(catalog_answer answer, const char *fragment, const char *label) {
    int matched = expect_cache_refusal_malformed(answer, fragment, label);
    cJSON_Delete(answer.tree);
    return matched;
}

#endif
