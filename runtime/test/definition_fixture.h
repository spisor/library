#ifndef RUNTIME_TEST_DEFINITION_FIXTURE_H
#define RUNTIME_TEST_DEFINITION_FIXTURE_H

#include <stdio.h>
#include <string.h>

#include "check.h"
#include "json_of.h"
#include "spisor/status.h"
#include "spisor/value.h"
#include "vector_reader.h"

/** A value tree read from JSON text, into an empty tree the caller releases; 0 when the text is no value. */
static inline int value_of_text(const char *text, spisor_value_tree *tree) {
    cJSON *wrapper = cJSON_CreateObject();
    cJSON *member = json_of((const uint8_t *)text, strlen(text));
    int read = 0;
    if (wrapper != NULL && member != NULL && cJSON_AddItemToObject(wrapper, "value", member)) {
        member = NULL;
        read = spisor_vector_value(wrapper, "value", tree);
    }
    cJSON_Delete(member);
    cJSON_Delete(wrapper);
    return read;
}

/** Whether a call answered `expected` with a refusal carrying a fix and, when `detail_part` is set, that text. */
static inline int check_refused(spisor_status status, spisor_status expected, const spisor_refusal *refusal,
                                const char *detail_part, const char *label) {
    int matches = status == expected && refusal->fix != NULL && refusal->detail != NULL &&
                  (detail_part == NULL || strstr(refusal->detail, detail_part) != NULL);
    if (!matches) {
        fprintf(stderr, "%s: expected status %d refusing with \"%s\", got status %d refusing with \"%s\"\n", label,
                (int)expected, detail_part == NULL ? "(any)" : detail_part, (int)status,
                refusal->detail == NULL ? "(none)" : refusal->detail);
        check_failures++;
    }
    return matches;
}

/** Whether a text ends with `suffix`. */
static inline int ends_with(const char *text, const char *suffix) {
    size_t text_length = text == NULL ? 0 : strlen(text);
    size_t suffix_length = strlen(suffix);
    return text != NULL && text_length >= suffix_length && strcmp(text + text_length - suffix_length, suffix) == 0;
}

#endif
