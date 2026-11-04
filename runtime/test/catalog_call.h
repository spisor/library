#ifndef RUNTIME_TEST_CATALOG_CALL_H
#define RUNTIME_TEST_CATALOG_CALL_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"

typedef int32_t (*catalog_function)(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

typedef struct catalog_answer {
    int32_t status;
    cJSON *tree;
} catalog_answer;

/** Calls function with argument, which it frees, and answers the status and the parsed answer. */
static inline catalog_answer call_catalog(catalog_function function, cJSON *argument) {
    char *text = cJSON_PrintUnformatted(argument);
    cJSON_Delete(argument);
    uint8_t *out = NULL;
    size_t out_len = 0;
    catalog_answer answer;
    answer.status = function((const uint8_t *)text, strlen(text), &out, &out_len);
    cJSON_free(text);
    answer.tree = out == NULL ? NULL : spisor_parse_json(out, out_len);
    spisor_release(out);
    return answer;
}

static inline cJSON *parsed(const char *json) {
    cJSON *tree = spisor_parse_json((const uint8_t *)json, strlen(json));
    if (tree == NULL) {
        fprintf(stderr, "a test's own JSON does not parse: %s\n", json);
        exit(2);
    }
    return tree;
}

static inline cJSON *read_json_file(const char *path) {
    size_t length;
    uint8_t *bytes = spisor_read_file(path, &length);
    cJSON *tree = bytes == NULL ? NULL : spisor_parse_json(bytes, length);
    free(bytes);
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

static inline int expect_refusal(catalog_answer answer, const char *fragment, const char *label) {
    const char *message = text_at(answer.tree, "message");
    int matched = answer.status == SPISOR_STATUS_FAILURE && message != NULL && text_at(answer.tree, "fix") != NULL &&
                  (fragment == NULL || strstr(message, fragment) != NULL);
    if (!matched) {
        fprintf(stderr, "%s: expected a refusal naming \"%s\", got status %d and \"%s\"\n", label,
                fragment == NULL ? "" : fragment, (int)answer.status, message == NULL ? "(none)" : message);
        check_failures++;
    }
    return matched;
}

#endif
