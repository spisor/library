#ifndef RUNTIME_TEST_UPDATES_CALL_H
#define RUNTIME_TEST_UPDATES_CALL_H

#include "spisor/allocation.h"
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "catalog_call.h"
#include "check.h"
#include "runtime/updates.h"
#include "spisor/status.h"
#include "spisor/value.h"
#include "vector_reader.h"

/**
 * @file
 * Calls the updates definition with a request a test writes as JSON, in the shape updates' operations take inside
 * runtime, and answers what the definition answered as that operation's JSON, so a test reads an answer the way
 * runtime's own callers do. A refusal answers {message, fix} with the definition's status. Test code only: the JSON is
 * the test's data, never the boundary.
 */

typedef enum updates_operation {
    UPDATES_APPLY,
    UPDATES_CHECK,
    UPDATES_ROLLBACK,
    UPDATES_RECORDED,
    UPDATES_RUN
} updates_operation;

/** What a JSON request holds once read into the shapes the definition takes; freed by updates_request_free. */
typedef struct updates_request {
    const char *state;
    const char *cache;
    const char *portable;
    spisor_value_tree settings;
    int has_settings;
    runtime_updates_default *defaults;
    size_t default_count;
    runtime_item_version *running;
    size_t running_count;
    runtime_item_version *damaged;
    size_t damaged_count;
    const char **libraries;
    size_t library_count;
} updates_request;

static inline const char *updates_text(const cJSON *object, const char *name) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
}

/** The members of a JSON object of texts as id and version pairs, which the caller frees. */
static inline runtime_item_version *updates_versions_of(const cJSON *object, size_t *count) {
    runtime_item_version *versions = spisor_memory_allocate_zeroed((size_t)cJSON_GetArraySize(object) + 1,
            sizeof *versions);
    const cJSON *member;
    *count = 0;
    CHECK(versions != NULL);
    cJSON_ArrayForEach(member, object) {
        versions[*count].id = member->string;
        versions[(*count)++].version = cJSON_GetStringValue(member);
    }
    return versions;
}

/** The members of a JSON object of texts as binding and plugin pairs, which the caller frees. */
static inline runtime_updates_default *updates_defaults_of(const cJSON *object, size_t *count) {
    runtime_updates_default *defaults = spisor_memory_allocate_zeroed((size_t)cJSON_GetArraySize(object) + 1,
            sizeof *defaults);
    const cJSON *member;
    *count = 0;
    CHECK(defaults != NULL);
    cJSON_ArrayForEach(member, object) {
        defaults[*count].binding = member->string;
        defaults[(*count)++].plugin = cJSON_GetStringValue(member);
    }
    return defaults;
}

static inline void updates_request_read(const cJSON *request, updates_request *read) {
    const cJSON *places = cJSON_GetObjectItemCaseSensitive(request, "places");
    const cJSON *libraries = cJSON_GetObjectItemCaseSensitive(request, "libraries");
    const cJSON *problems = cJSON_GetObjectItemCaseSensitive(request, "problems");
    const cJSON *entry;
    memset(read, 0, sizeof *read);
    read->settings = (spisor_value_tree)SPISOR_INIT(spisor_value_tree);
    read->state = updates_text(places, "state");
    read->cache = updates_text(places, "cache");
    read->portable = updates_text(places, "portable");
    if (cJSON_GetObjectItemCaseSensitive(request, "settings") != NULL) {
        CHECK(spisor_vector_value(request, "settings", &read->settings));
        read->has_settings = 1;
    }
    read->defaults = updates_defaults_of(cJSON_GetObjectItemCaseSensitive(request, "defaults"), &read->default_count);
    read->running = updates_versions_of(cJSON_GetObjectItemCaseSensitive(request, "running"), &read->running_count);
    read->damaged = spisor_memory_allocate_zeroed((size_t)cJSON_GetArraySize(problems) + 1, sizeof *read->damaged);
    CHECK(read->damaged != NULL);
    cJSON_ArrayForEach(entry, problems) {
        read->damaged[read->damaged_count].id = updates_text(entry, "id");
        read->damaged[read->damaged_count++].version = updates_text(entry, "version");
    }
    if (cJSON_IsArray(libraries)) {
        read->libraries = spisor_memory_allocate_zeroed((size_t)cJSON_GetArraySize(libraries) + 1,
                sizeof *read->libraries);
        CHECK(read->libraries != NULL);
        cJSON_ArrayForEach(entry, libraries) {
            read->libraries[read->library_count++] = cJSON_GetStringValue(entry);
        }
    }
}

static inline void updates_request_free(updates_request *read) {
    spisor_value_release(&read->settings);
    spisor_memory_release(read->defaults);
    spisor_memory_release(read->running);
    spisor_memory_release(read->damaged);
    spisor_memory_release((void *)read->libraries);
}

static inline void updates_add_text(cJSON *object, const char *name, const char *text) {
    if (text != NULL) {
        CHECK(cJSON_AddStringToObject(object, name, text) != NULL);
    }
}

static inline cJSON *updates_versions_json(const runtime_item_version *versions, size_t count) {
    cJSON *object = cJSON_CreateObject();
    for (size_t index = 0; index < count; index++) {
        updates_add_text(object, versions[index].id, versions[index].version);
    }
    return object;
}

static inline cJSON *updates_problems_json(const runtime_updates_problem *problems, size_t count) {
    cJSON *array = cJSON_CreateArray();
    for (size_t index = 0; index < count; index++) {
        cJSON *entry = cJSON_CreateObject();
        updates_add_text(entry, "id", problems[index].id);
        updates_add_text(entry, "message", problems[index].detail);
        updates_add_text(entry, "fix", problems[index].fix);
        CHECK(cJSON_AddItemToArray(array, entry));
    }
    return array;
}

static inline cJSON *updates_texts_json(const char *const *texts, size_t count) {
    cJSON *array = cJSON_CreateArray();
    for (size_t index = 0; index < count; index++) {
        CHECK(cJSON_AddItemToArray(array, cJSON_CreateString(texts[index])));
    }
    return array;
}

static inline cJSON *updates_check_json(const runtime_updates_source_check *sources, size_t source_count,
                                        const char *const *fetched, size_t fetched_count,
                                        const runtime_updates_problem *refused, size_t refused_count,
                                        const char *const *removed, size_t removed_count) {
    static const char *const outcomes[] = {"", "skipped", "unchanged", "changed", "failed"};
    cJSON *tree = cJSON_CreateObject();
    cJSON *checked = cJSON_AddArrayToObject(tree, "checked");
    for (size_t index = 0; index < source_count; index++) {
        cJSON *entry = cJSON_CreateObject();
        updates_add_text(entry, "kind", outcomes[sources[index].outcome < 5 ? sources[index].outcome : 0]);
        updates_add_text(entry, "detail", sources[index].detail);
        CHECK(cJSON_AddItemToArray(checked, entry));
    }
    cJSON_AddItemToObject(tree, "fetched", updates_texts_json(fetched, fetched_count));
    cJSON_AddItemToObject(tree, "refused", updates_problems_json(refused, refused_count));
    cJSON_AddItemToObject(tree, "removed", updates_texts_json(removed, removed_count));
    return tree;
}

static inline void updates_add_references(cJSON *tree, const runtime_item_version *run, size_t run_count,
                                          const runtime_item_version *next, size_t next_count,
                                          const runtime_item_version *rollback, size_t rollback_count) {
    cJSON_AddItemToObject(tree, "run", updates_versions_json(run, run_count));
    cJSON_AddItemToObject(tree, "next", updates_versions_json(next, next_count));
    cJSON_AddItemToObject(tree, "rollback", updates_versions_json(rollback, rollback_count));
}

static inline spisor_status updates_apply_call(const runtime_updates_definition *updates, const cJSON *request,
                                               const updates_request *read, cJSON **tree, spisor_refusal *refusal) {
    runtime_updates_apply_argument argument = SPISOR_INIT(runtime_updates_apply_argument);
    runtime_updates_apply_answer answer = SPISOR_INIT(runtime_updates_apply_answer);
    spisor_status status;
    argument.state = read->state;
    argument.cache = read->cache;
    argument.host_id = updates_text(request, "hostId");
    argument.settings = read->has_settings ? &read->settings.root : NULL;
    argument.defaults = read->defaults;
    argument.default_count = read->default_count;
    argument.running = read->running;
    argument.running_count = read->running_count;
    argument.writes_boot_hint = cJSON_HasObjectItem(request, "libraries") ? 1 : 0;
    argument.libraries = read->libraries;
    argument.library_count = read->library_count;
    argument.platform = updates_text(request, "platform");
    status = updates->apply(updates->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        *tree = cJSON_CreateObject();
        cJSON_AddItemToObject(*tree, "plugins", updates_versions_json(answer.plugins, answer.plugin_count));
        updates_add_references(*tree, answer.run, answer.run_count, answer.next, answer.next_count, answer.rollback,
                               answer.rollback_count);
        cJSON_AddItemToObject(*tree, "unrecorded", updates_problems_json(answer.unrecorded, answer.unrecorded_count));
        updates_add_text(*tree, "runRecord", answer.run_record);
    }
    updates->release_apply_answer(updates->context, &answer);
    return status;
}

static inline spisor_status updates_check_call(const runtime_updates_definition *updates, const cJSON *request,
                                               const updates_request *read, cJSON **tree, spisor_refusal *refusal) {
    runtime_updates_check_argument argument = SPISOR_INIT(runtime_updates_check_argument);
    runtime_updates_check_answer answer = SPISOR_INIT(runtime_updates_check_answer);
    spisor_status status;
    argument.state = read->state;
    argument.cache = read->cache;
    argument.host_id = updates_text(request, "hostId");
    argument.now = updates_text(request, "now");
    argument.settings = read->has_settings ? &read->settings.root : NULL;
    argument.defaults = read->defaults;
    argument.default_count = read->default_count;
    argument.damaged = read->damaged;
    argument.damaged_count = read->damaged_count;
    argument.platform = updates_text(request, "platform");
    status = updates->check(updates->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        *tree = updates_check_json(answer.sources, answer.source_count, answer.fetched, answer.fetched_count,
                                   answer.refused, answer.refused_count, answer.removed, answer.removed_count);
    }
    updates->release_check_answer(updates->context, &answer);
    return status;
}

static inline spisor_status updates_rollback_call(const runtime_updates_definition *updates, const cJSON *request,
                                                  const updates_request *read, cJSON **tree,
                                                  spisor_refusal *refusal) {
    runtime_updates_rollback_argument argument = SPISOR_INIT(runtime_updates_rollback_argument);
    runtime_updates_rollback_answer answer = SPISOR_INIT(runtime_updates_rollback_answer);
    spisor_status status;
    argument.state = read->state;
    argument.cache = read->cache;
    argument.host_id = updates_text(request, "hostId");
    argument.item = updates_text(request, "item");
    status = updates->rollback(updates->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        *tree = cJSON_CreateObject();
        updates_add_references(*tree, answer.run, answer.run_count, answer.next, answer.next_count, answer.rollback,
                               answer.rollback_count);
    }
    updates->release_rollback_answer(updates->context, &answer);
    return status;
}

static inline spisor_status updates_recorded_call(const runtime_updates_definition *updates, const cJSON *request,
                                                  const updates_request *read, cJSON **tree,
                                                  spisor_refusal *refusal) {
    runtime_updates_recorded_argument argument = SPISOR_INIT(runtime_updates_recorded_argument);
    runtime_updates_recorded_answer answer = SPISOR_INIT(runtime_updates_recorded_answer);
    spisor_status status;
    argument.state = read->state;
    argument.cache = read->cache;
    argument.host_id = updates_text(request, "hostId");
    status = updates->recorded(updates->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        *tree = cJSON_CreateObject();
        cJSON_AddItemToObject(*tree, "run", updates_versions_json(answer.run, answer.run_count));
    }
    updates->release_recorded_answer(updates->context, &answer);
    return status;
}

static inline cJSON *updates_run_json(const runtime_updates_run_answer *answer) {
    static const char *const operations[] = {"", "update", "remove"};
    cJSON *tree = cJSON_CreateObject();
    cJSON *checks = cJSON_AddObjectToObject(tree, "checks");
    cJSON *applications = cJSON_AddObjectToObject(tree, "applications");
    for (size_t index = 0; index < answer->check_count; index++) {
        const runtime_updates_host_check *each = &answer->checks[index];
        CHECK(cJSON_AddItemToObject(checks, each->host_id,
                                    updates_check_json(each->sources, each->source_count, each->fetched,
                                                       each->fetched_count, each->refused, each->refused_count,
                                                       each->removed, each->removed_count)));
    }
    for (size_t index = 0; index < answer->application_count; index++) {
        const runtime_updates_application *each = &answer->applications[index];
        cJSON *entry = cJSON_AddObjectToObject(applications, each->loader);
        updates_add_text(entry, "operation", operations[each->operation < 3 ? each->operation : 0]);
        updates_add_text(entry, "version", each->version);
        if (each->unrecorded_detail != NULL) {
            cJSON *unrecorded = cJSON_AddObjectToObject(entry, "unrecorded");
            updates_add_text(unrecorded, "message", each->unrecorded_detail);
            updates_add_text(unrecorded, "fix", each->unrecorded_fix);
        }
    }
    cJSON_AddItemToObject(tree, "refused", updates_problems_json(answer->refused, answer->refused_count));
    return tree;
}

static inline spisor_status updates_run_call(const runtime_updates_definition *updates, const cJSON *request,
                                             const updates_request *read, cJSON **tree, spisor_refusal *refusal) {
    runtime_updates_run_argument argument = SPISOR_INIT(runtime_updates_run_argument);
    runtime_updates_run_answer answer = SPISOR_INIT(runtime_updates_run_answer);
    spisor_status status;
    argument.portable = read->portable;
    argument.state = read->state;
    argument.cache = read->cache;
    argument.now = updates_text(request, "now");
    status = updates->run(updates->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        *tree = updates_run_json(&answer);
    }
    updates->release_run_answer(updates->context, &answer);
    return status;
}

/** Calls `operation` with `request`, which it frees, answering the definition's status and the answer as JSON. */
static inline catalog_answer updates_call(updates_operation operation, cJSON *request) {
    const runtime_updates_definition *updates = runtime_updates_definition_of();
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    catalog_answer result = {0, NULL};
    updates_request read;
    spisor_status status;
    updates_request_read(request, &read);
    switch (operation) {
    case UPDATES_APPLY:
        status = updates_apply_call(updates, request, &read, &result.tree, &refusal);
        break;
    case UPDATES_CHECK:
        status = updates_check_call(updates, request, &read, &result.tree, &refusal);
        break;
    case UPDATES_ROLLBACK:
        status = updates_rollback_call(updates, request, &read, &result.tree, &refusal);
        break;
    case UPDATES_RECORDED:
        status = updates_recorded_call(updates, request, &read, &result.tree, &refusal);
        break;
    default:
        status = updates_run_call(updates, request, &read, &result.tree, &refusal);
        break;
    }
    if (status != SPISOR_OK) {
        result.tree = cJSON_CreateObject();
        updates_add_text(result.tree, "message", refusal.detail);
        updates_add_text(result.tree, "fix", refusal.fix);
    }
    updates->release_refusal(updates->context, &refusal);
    updates_request_free(&read);
    cJSON_Delete(request);
    result.status = (int32_t)status;
    return result;
}

#endif
