#ifndef RUNTIME_TEST_MARKETPLACE_CALL_H
#define RUNTIME_TEST_MARKETPLACE_CALL_H

#include <stdlib.h>
#include <string.h>

#include "catalog_call.h"
#include "check.h"
#include "json_value.h"
#include "runtime/marketplace.h"
#include "vector_reader.h"

/**
 * @file
 * Calls the marketplace definition with an argument a test writes as JSON, in the shape the marketplace documents
 * use, and answers as catalog_call.h does: SUCCESS with the answer as JSON, FAILURE with {message, fix}, or
 * OUT_OF_MEMORY, the definition's own status kept in marketplace_last_status for a test that tells MALFORMED from
 * REFUSED. A lookup that finds nothing answers {notFound: true}, a remember {remembered}, {settled} or
 * {needsUserAcceptance, was, now}. Test code only: the JSON is the test's data, never the boundary.
 */

typedef enum marketplace_operation {
    MARKETPLACE_BUILD,
    MARKETPLACE_SOURCES,
    MARKETPLACE_READ,
    MARKETPLACE_LOOKUP,
    MARKETPLACE_REMEMBER
} marketplace_operation;

static spisor_status marketplace_last_status = SPISOR_OK;

static inline const runtime_marketplace_definition *marketplace(void) {
    return runtime_marketplace_definition_of();
}

static inline const char *marketplace_text(const cJSON *object, const char *name) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
}

/** A member as a value tree into `tree`, NULL when the argument has no such member. */
static inline const spisor_value *marketplace_value(const cJSON *object, const char *name, spisor_value_tree *tree) {
    tree->size = (uint32_t)sizeof *tree;
    if (cJSON_GetObjectItemCaseSensitive(object, name) == NULL) {
        return NULL;
    }
    CHECK(spisor_vector_value(object, name, tree));
    return &tree->root;
}

static inline cJSON *marketplace_json_of(const spisor_value *value) {
    cJSON *json = NULL;
    CHECK(value != NULL && spisor_json_of_value(value, &json) == SPISOR_OK);
    return json;
}

static inline void marketplace_add_text(cJSON *object, const char *name, const char *text) {
    if (text != NULL) {
        cJSON_AddStringToObject(object, name, text);
    }
}

static inline cJSON *marketplace_build_tree(const cJSON *argument, spisor_refusal *refusal) {
    spisor_value_tree list = SPISOR_INIT(spisor_value_tree);
    spisor_value_tree previous = SPISOR_INIT(spisor_value_tree);
    runtime_marketplace_build_argument call = SPISOR_INIT(runtime_marketplace_build_argument);
    runtime_marketplace_build_answer answer = SPISOR_INIT(runtime_marketplace_build_answer);
    cJSON *tree = NULL;
    call.list = marketplace_value(argument, "list", &list);
    call.built = marketplace_text(argument, "built");
    call.previous = marketplace_value(argument, "previous", &previous);
    call.token = marketplace_text(argument, "token");
    call.token_host = marketplace_text(argument, "tokenHost");
    call.api_base = marketplace_text(argument, "apiBase");
    marketplace_last_status = marketplace()->build(marketplace()->context, &call, &answer, refusal);
    if (marketplace_last_status == SPISOR_OK) {
        cJSON *skipped;
        tree = cJSON_CreateObject();
        cJSON_AddItemToObject(tree, "copy", marketplace_json_of(answer.copy));
        skipped = cJSON_AddArrayToObject(tree, "skipped");
        for (size_t index = 0; index < answer.skipped_count; index++) {
            cJSON *entry = cJSON_CreateObject();
            cJSON_AddStringToObject(entry, "id", answer.skipped[index].id);
            cJSON_AddStringToObject(entry, "reason", answer.skipped[index].reason);
            cJSON_AddItemToArray(skipped, entry);
        }
    }
    marketplace()->release_build_answer(marketplace()->context, &answer);
    spisor_value_release(&list);
    spisor_value_release(&previous);
    return tree;
}

static inline cJSON *marketplace_sources_tree(const cJSON *argument, spisor_refusal *refusal) {
    spisor_value_tree settings = SPISOR_INIT(spisor_value_tree);
    runtime_marketplace_sources_argument call = SPISOR_INIT(runtime_marketplace_sources_argument);
    runtime_marketplace_sources_answer answer = SPISOR_INIT(runtime_marketplace_sources_answer);
    cJSON *tree = NULL;
    call.settings = marketplace_value(argument, "settings", &settings);
    marketplace_last_status = marketplace()->sources(marketplace()->context, &call, &answer, refusal);
    if (marketplace_last_status == SPISOR_OK) {
        cJSON *sources;
        cJSON *skipped;
        tree = cJSON_CreateObject();
        sources = cJSON_AddArrayToObject(tree, "sources");
        skipped = cJSON_AddArrayToObject(tree, "skipped");
        for (size_t index = 0; index < answer.source_count; index++) {
            const runtime_marketplace_source *source = &answer.sources[index];
            cJSON *entry = cJSON_CreateObject();
            CHECK((source->kind == RUNTIME_MARKETPLACE_SOURCE_URL) == (source->url != NULL));
            CHECK((source->kind == RUNTIME_MARKETPLACE_SOURCE_PROVIDER) == (source->provider != NULL));
            cJSON_AddStringToObject(entry, "id", source->id);
            marketplace_add_text(entry, "url", source->url);
            marketplace_add_text(entry, "provider", source->provider);
            cJSON_AddItemToArray(sources, entry);
        }
        for (size_t index = 0; index < answer.skipped_count; index++) {
            cJSON *entry = cJSON_CreateObject();
            cJSON_AddNumberToObject(entry, "index", (double)answer.skipped[index].index);
            cJSON_AddStringToObject(entry, "detail", answer.skipped[index].detail);
            cJSON_AddItemToArray(skipped, entry);
        }
    }
    marketplace()->release_sources_answer(marketplace()->context, &answer);
    spisor_value_release(&settings);
    return tree;
}

static inline cJSON *marketplace_read_tree(const cJSON *argument, spisor_refusal *refusal) {
    const cJSON *source = cJSON_GetObjectItemCaseSensitive(argument, "source");
    runtime_marketplace_read_argument call = SPISOR_INIT(runtime_marketplace_read_argument);
    runtime_marketplace_copy_answer answer = SPISOR_INIT(runtime_marketplace_copy_answer);
    cJSON *tree = NULL;
    call.id = marketplace_text(source, "id");
    call.url = marketplace_text(source, "url");
    call.text = marketplace_text(argument, "text");
    call.destination = marketplace_text(argument, "destination");
    marketplace_last_status = marketplace()->read(marketplace()->context, &call, &answer, refusal);
    if (marketplace_last_status == SPISOR_OK) {
        tree = cJSON_CreateObject();
        cJSON_AddItemToObject(tree, "copy", marketplace_json_of(answer.copy));
    }
    marketplace()->release_copy_answer(marketplace()->context, &answer);
    return tree;
}

static inline cJSON *marketplace_lookup_tree(const cJSON *argument, spisor_refusal *refusal) {
    const cJSON *copies = cJSON_GetObjectItemCaseSensitive(argument, "copies");
    size_t count = (size_t)cJSON_GetArraySize(copies);
    runtime_marketplace_copy *pairs = calloc(count + 1, sizeof *pairs);
    spisor_value_tree *trees = calloc(count + 1, sizeof *trees);
    runtime_marketplace_lookup_argument call = SPISOR_INIT(runtime_marketplace_lookup_argument);
    runtime_marketplace_lookup_answer answer = SPISOR_INIT(runtime_marketplace_lookup_answer);
    const cJSON *pair;
    cJSON *tree = NULL;
    cJSON_ArrayForEach(pair, copies) {
        pairs[call.copy_count].source = marketplace_text(cJSON_GetObjectItemCaseSensitive(pair, "source"), "id");
        pairs[call.copy_count].copy = marketplace_value(pair, "copy", &trees[call.copy_count]);
        call.copy_count++;
    }
    call.copies = pairs;
    call.name = marketplace_text(argument, "name");
    marketplace_last_status = marketplace()->lookup(marketplace()->context, &call, &answer, refusal);
    if (marketplace_last_status == SPISOR_NONE) {
        tree = cJSON_CreateObject();
        cJSON_AddTrueToObject(tree, "notFound");
    } else if (marketplace_last_status == SPISOR_OK && answer.match == RUNTIME_MARKETPLACE_AMBIGUOUS) {
        cJSON *ambiguous;
        tree = cJSON_CreateObject();
        ambiguous = cJSON_AddArrayToObject(tree, "ambiguous");
        for (size_t index = 0; index < answer.ambiguous_count; index++) {
            cJSON_AddItemToArray(ambiguous, cJSON_CreateString(answer.ambiguous[index]));
        }
    } else if (marketplace_last_status == SPISOR_OK) {
        CHECK(answer.match == RUNTIME_MARKETPLACE_FOUND);
        tree = cJSON_CreateObject();
        marketplace_add_text(tree, "id", answer.id);
        marketplace_add_text(tree, "marketplace", answer.marketplace);
    }
    marketplace()->release_lookup_answer(marketplace()->context, &answer);
    for (size_t index = 0; index < count; index++) {
        spisor_value_release(&trees[index]);
    }
    free(trees);
    free(pairs);
    return tree;
}

static inline cJSON *marketplace_remember_tree(const cJSON *argument, spisor_refusal *refusal) {
    runtime_marketplace_remember_argument call = SPISOR_INIT(runtime_marketplace_remember_argument);
    runtime_marketplace_remember_answer answer = SPISOR_INIT(runtime_marketplace_remember_answer);
    cJSON *tree = NULL;
    call.state = marketplace_text(argument, "state");
    call.marketplace = marketplace_text(argument, "marketplace");
    call.name = marketplace_text(argument, "name");
    call.id = marketplace_text(argument, "id");
    call.accept = marketplace_text(argument, "accept");
    call.check_only = cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(argument, "record")) ? 1 : 0;
    marketplace_last_status = marketplace()->remember(marketplace()->context, &call, &answer, refusal);
    if (marketplace_last_status == SPISOR_OK) {
        static const char *const memories[] = {"", "remembered", "settled", "needsUserAcceptance"};
        tree = cJSON_CreateObject();
        cJSON_AddTrueToObject(tree, memories[answer.memory <= 3 ? answer.memory : 0]);
        marketplace_add_text(tree, "was", answer.was);
        marketplace_add_text(tree, "now", answer.now);
    }
    marketplace()->release_remember_answer(marketplace()->context, &answer);
    return tree;
}

/** Calls one marketplace operation with the argument a test wrote, deleting the argument. */
static inline catalog_answer marketplace_call(marketplace_operation operation, cJSON *argument) {
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    catalog_answer answer;
    switch (operation) {
    case MARKETPLACE_BUILD:
        answer.tree = marketplace_build_tree(argument, &refusal);
        break;
    case MARKETPLACE_SOURCES:
        answer.tree = marketplace_sources_tree(argument, &refusal);
        break;
    case MARKETPLACE_READ:
        answer.tree = marketplace_read_tree(argument, &refusal);
        break;
    case MARKETPLACE_LOOKUP:
        answer.tree = marketplace_lookup_tree(argument, &refusal);
        break;
    default:
        answer.tree = marketplace_remember_tree(argument, &refusal);
        break;
    }
    answer.status = SPISOR_STATUS_SUCCESS;
    if (marketplace_last_status == SPISOR_OUT_OF_MEMORY) {
        answer.status = SPISOR_STATUS_OUT_OF_MEMORY;
    } else if (marketplace_last_status == SPISOR_MALFORMED || marketplace_last_status == SPISOR_REFUSED) {
        answer.status = SPISOR_STATUS_FAILURE;
        answer.tree = cJSON_CreateObject();
        marketplace_add_text(answer.tree, "message", refusal.detail);
        marketplace_add_text(answer.tree, "fix", refusal.fix);
    }
    marketplace()->release_refusal(marketplace()->context, &refusal);
    cJSON_Delete(argument);
    return answer;
}

#endif
