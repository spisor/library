#ifndef RUNTIME_TEST_CATALOG_CALL_H
#define RUNTIME_TEST_CATALOG_CALL_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spisor/allocation.h"
#include "check.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"
#include "spisor/status.h"

typedef struct catalog_answer {
    int32_t status;
    cJSON *tree;
} catalog_answer;

static inline cJSON *parsed(const char *json) {
    cJSON *tree = spisor_parse_json_reporting((const uint8_t *)json, strlen(json), &(int){0});
    if (tree == NULL) {
        fprintf(stderr, "a test's own JSON does not parse: %s\n", json);
        exit(2);
    }
    return tree;
}

static inline cJSON *read_json_file(const char *path) {
    size_t length;
    uint8_t *bytes = spisor_read_file_reporting(path, &length, &(spisor_read_outcome){SPISOR_READ_FAILED});
    cJSON *tree = bytes == NULL ? NULL : spisor_parse_json_reporting(bytes, length, &(int){0});
    spisor_memory_release(bytes);
    if (tree == NULL) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    return tree;
}

static inline const char *text_at(const cJSON *tree, const char *member) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(tree, member);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

/** The value at a path of member names separated by slashes, or NULL. */
static inline const cJSON *value_at(const cJSON *tree, const char *slashed) {
    char name[256];
    const cJSON *at = tree;
    while (at != NULL && *slashed != 0) {
        size_t length = strcspn(slashed, "/");
        if (length >= sizeof name) {
            return NULL;
        }
        memcpy(name, slashed, length);
        name[length] = 0;
        at = cJSON_GetObjectItemCaseSensitive(at, name);
        slashed += length + (slashed[length] == '/');
    }
    return at;
}

/** Whether a status is a definition's refusal: MALFORMED or REFUSED. */
static inline int is_refusal_status(int32_t status) {
    return status == SPISOR_MALFORMED || status == SPISOR_REFUSED;
}

static inline int expect_refusal(catalog_answer answer, const char *fragment, const char *label) {
    const char *message = text_at(answer.tree, "message");
    int matched = is_refusal_status(answer.status) && message != NULL && text_at(answer.tree, "fix") != NULL &&
                  (fragment == NULL || strstr(message, fragment) != NULL);
    if (!matched) {
        fprintf(stderr, "%s: expected a refusal naming \"%s\", got status %d and \"%s\"\n", label,
                fragment == NULL ? "" : fragment, (int)answer.status, message == NULL ? "(none)" : message);
        check_failures++;
    }
    return matched;
}

/** expect_refusal on an answer no caller keeps, releasing its tree. */
static inline int expect_refusal_released(catalog_answer answer, const char *fragment, const char *label) {
    int matched = expect_refusal(answer, fragment, label);
    cJSON_Delete(answer.tree);
    return matched;
}

#endif
