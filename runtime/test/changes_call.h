#ifndef RUNTIME_TEST_CHANGES_CALL_H
#define RUNTIME_TEST_CHANGES_CALL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spisor/allocation.h"
#include "cJSON.h"
#include "check.h"
#include "runtime/changes.h"
#include "spisor/status.h"
#include "spisor/value.h"
#include "vector_reader.h"

/**
 * @file
 * Calls the changes definition with a change a test writes as JSON, in the shape the record stores, and answers what
 * the definition answered as JSON a test reads: {recorded, sequence?} for apply, {reversed, drifted: [{sequence, kind,
 * detail}]} for reverse. Test code only: the JSON is the test's data, never the boundary.
 */

/** What a call answered: its status, the answer as JSON (NULL unless OK), and the refusal's texts. */
typedef struct changes_result {
    spisor_status status;
    cJSON *tree;
    char detail[1024];
    char fix[1024];
} changes_result;

static inline const char *changes_member_text(const cJSON *object, const char *name) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
}

static inline uint32_t changes_kind_of(const char *kind) {
    static const char *const names[] = {"file", "directory", "json-key", "profile-line", "path-entry"};
    for (uint32_t index = 0; kind != NULL && index < 5; index++) {
        if (strcmp(kind, names[index]) == 0) {
            return index + 1;
        }
    }
    return 0;
}

static inline void changes_keep_refusal(changes_result *result, const spisor_refusal *refusal) {
    snprintf(result->detail, sizeof result->detail, "%s", refusal->detail == NULL ? "" : refusal->detail);
    snprintf(result->fix, sizeof result->fix, "%s", refusal->fix == NULL ? "" : refusal->fix);
}

/** Applies the change a test wrote for host in state, deleting the change. */
static inline changes_result changes_apply_call(const char *state, const char *host, cJSON *json) {
    const runtime_changes_definition *changes = runtime_changes_definition_of();
    runtime_change change = SPISOR_INIT(runtime_change);
    runtime_changes_apply_argument argument = SPISOR_INIT(runtime_changes_apply_argument);
    runtime_changes_apply_answer answer = SPISOR_INIT(runtime_changes_apply_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_value_tree value = SPISOR_INIT(spisor_value_tree);
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(json, "key");
    const char **segments = spisor_memory_allocate_zeroed((size_t)cJSON_GetArraySize(key) + 1, sizeof *segments);
    changes_result result;
    memset(&result, 0, sizeof result);
    change.kind = changes_kind_of(changes_member_text(json, "kind"));
    change.path = changes_member_text(json, "path");
    change.content = changes_member_text(json, "content");
    change.executable = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "executable"));
    change.file = changes_member_text(json, "file");
    for (const cJSON *segment = key == NULL ? NULL : key->child; segment != NULL; segment = segment->next) {
        segments[change.key_count++] = cJSON_GetStringValue(segment);
    }
    change.key = segments;
    if (cJSON_GetObjectItemCaseSensitive(json, "value") != NULL) {
        CHECK(spisor_vector_value(json, "value", &value));
        change.value = &value.root;
    }
    change.create = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "create"));
    change.line = changes_member_text(json, "line");
    change.directory = changes_member_text(json, "directory");
    change.home = changes_member_text(json, "home");
    change.source = changes_member_text(json, "source");
    change.shell = changes_member_text(json, "shell");
    argument.state = state;
    argument.host = host;
    argument.change = &change;
    result.status = changes->apply(changes->context, &argument, &answer, &refusal);
    if (result.status == SPISOR_OK) {
        result.tree = cJSON_CreateObject();
        cJSON_AddBoolToObject(result.tree, "recorded", answer.recorded != 0);
        if (answer.sequence > 0) {
            cJSON_AddNumberToObject(result.tree, "sequence", (double)answer.sequence);
        }
    }
    changes_keep_refusal(&result, &refusal);
    changes->release_refusal(changes->context, &refusal);
    spisor_value_release(&value);
    spisor_memory_release(segments);
    cJSON_Delete(json);
    return result;
}

/** Reverses every change of host in state. */
static inline changes_result changes_reverse_call(const char *state, const char *host) {
    const runtime_changes_definition *changes = runtime_changes_definition_of();
    runtime_changes_reverse_argument argument = SPISOR_INIT(runtime_changes_reverse_argument);
    runtime_changes_reverse_answer answer = SPISOR_INIT(runtime_changes_reverse_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    changes_result result;
    memset(&result, 0, sizeof result);
    argument.state = state;
    argument.host = host;
    result.status = changes->reverse(changes->context, &argument, &answer, &refusal);
    if (result.status == SPISOR_OK) {
        cJSON *drifted;
        result.tree = cJSON_CreateObject();
        cJSON_AddNumberToObject(result.tree, "reversed", (double)answer.reversed);
        drifted = cJSON_AddArrayToObject(result.tree, "drifted");
        for (size_t index = 0; index < answer.drifted_count; index++) {
            cJSON *entry = cJSON_CreateObject();
            cJSON_AddNumberToObject(entry, "sequence", (double)answer.drifted[index].sequence);
            cJSON_AddStringToObject(entry, "kind", answer.drifted[index].kind);
            cJSON_AddStringToObject(entry, "detail", answer.drifted[index].detail);
            cJSON_AddItemToArray(drifted, entry);
        }
    }
    changes_keep_refusal(&result, &refusal);
    changes->release_reverse_answer(changes->context, &answer);
    changes->release_refusal(changes->context, &refusal);
    return result;
}

#endif
