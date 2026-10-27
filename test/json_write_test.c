#include <stdio.h>
#include <stdlib.h>

#include "check.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"
#include "spisor/json_write.h"

static cJSON *load_vectors(void) {
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(SPISOR_JSON_WRITE_VECTORS_PATH, &length);
    if (bytes == NULL) {
        fprintf(stderr, "cannot read %s\n", SPISOR_JSON_WRITE_VECTORS_PATH);
        return NULL;
    }
    cJSON *vectors = cJSON_ParseWithLength((const char *)bytes, length);
    free(bytes);
    return vectors;
}

static void check_vector(const cJSON *vector, int index) {
    char label[32];
    snprintf(label, sizeof(label), "vector %d", index);
    const char *input = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(vector, "input"));
    const char *expected = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(vector, "expected"));
    CHECK(input != NULL && expected != NULL);
    if (input == NULL || expected == NULL) {
        return;
    }
    cJSON *parsed = spisor_parse_json((const uint8_t *)input, strlen(input));
    CHECK(parsed != NULL);
    if (parsed == NULL) {
        fprintf(stderr, "%s: cannot parse \"%s\"\n", label, input);
        return;
    }
    char *actual = spisor_json_stringify_pretty(parsed);
    if (!check_text(actual, expected, label)) {
        fprintf(stderr, "  for input %s\n", input);
    }
    free(actual);
    cJSON_Delete(parsed);
}

static void matches_every_vector(void) {
    cJSON *vectors = load_vectors();
    CHECK(vectors != NULL);
    if (vectors == NULL) {
        return;
    }
    const cJSON *vector = NULL;
    int index = 0;
    cJSON_ArrayForEach(vector, cJSON_GetObjectItemCaseSensitive(vectors, "stringify")) {
        check_vector(vector, index++);
    }
    CHECK(index > 0);
    cJSON_Delete(vectors);
}

int main(void) {
    matches_every_vector();
    return check_summary("json_write_test");
}
