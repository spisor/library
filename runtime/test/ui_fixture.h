#ifndef RUNTIME_TEST_UI_FIXTURE_H
#define RUNTIME_TEST_UI_FIXTURE_H

#include <stdio.h>
#include <string.h>

#include "catalog_call.h"
#include "check.h"
#include "ui.h"

/** The manifest of one of the fixture plugins under fixtures/ui. */
static inline cJSON *fixture_manifest(const char *plugin) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.json", RUNTIME_UI_FIXTURES_PATH, plugin);
    return read_json_file(path);
}

/** The descriptions array of the named fixture plugins, in the order given. */
static inline cJSON *fixture_descriptions(const char *const *plugins, size_t count) {
    cJSON *descriptions = cJSON_CreateArray();
    for (size_t index = 0; index < count; index++) {
        cJSON_AddItemToArray(descriptions, fixture_manifest(plugins[index]));
    }
    return descriptions;
}

/** Fails when actual, which it frees, is not the JSON expected. */
static inline void check_json(cJSON *actual, const char *expected, const char *label) {
    cJSON *wanted = parsed(expected);
    if (!cJSON_Compare(actual, wanted, 1)) {
        char *text = actual == NULL ? NULL : cJSON_PrintUnformatted(actual);
        fprintf(stderr, "%s: expected %s, got %s\n", label, expected, text == NULL ? "(null)" : text);
        cJSON_free(text);
        check_failures++;
    }
    cJSON_Delete(wanted);
    cJSON_Delete(actual);
}

/** Fails when the value at slashed in tree is not the JSON expected. */
static inline void check_json_at(const cJSON *tree, const char *slashed, const char *expected, const char *label) {
    const cJSON *found = value_at(tree, slashed);
    check_json(found == NULL ? NULL : cJSON_Duplicate(found, 1), expected, label);
}

/** Answers the successful tree of function called with argument, which it frees; NULL after a failed check. */
static inline cJSON *successful(catalog_function function, cJSON *argument, const char *label) {
    catalog_answer answer = call_catalog(function, argument);
    if (answer.status != SPISOR_STATUS_SUCCESS) {
        const char *message = text_at(answer.tree, "message");
        fprintf(stderr, "%s: expected success, got status %d: %s\n", label, (int)answer.status,
                message == NULL ? "(no message)" : message);
        check_failures++;
        cJSON_Delete(answer.tree);
        return NULL;
    }
    return answer.tree;
}

#endif
