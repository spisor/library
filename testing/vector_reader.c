#include "vector_reader.h"
#include "allocation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spisor/file.h"
#include "spisor/json_kind.h"
#include "json_value.h"

typedef struct status_name {
    const char *name;
    spisor_status status;
} status_name;

static const status_name STATUS_NAMES[] = {
    {"OK", SPISOR_OK},
    {"NONE", SPISOR_NONE},
    {"MALFORMED", SPISOR_MALFORMED},
    {"REFUSED", SPISOR_REFUSED},
    {"OUT_OF_MEMORY", SPISOR_OUT_OF_MEMORY},
};

static int unread(const char *key, const char *why) {
    fprintf(stderr, "vector member \"%s\": %s\n", key, why);
    return 0;
}

cJSON *spisor_vector_read(const char *path) {
    size_t length = 0;
    uint8_t *bytes = spisor_read_file_reporting(path, &length, &(spisor_read_outcome){SPISOR_READ_FAILED});
    cJSON *vector;
    if (bytes == NULL) {
        fprintf(stderr, "vector %s cannot be read\n", path);
        return NULL;
    }
    vector = spisor_parse_json(bytes, length);
    spisor_memory_release(bytes);
    if (vector == NULL) {
        fprintf(stderr, "vector %s is not JSON\n", path);
    }
    return vector;
}

const cJSON *spisor_vector_member(const cJSON *object, const char *key) {
    const cJSON *member = cJSON_IsObject(object) ? cJSON_GetObjectItemCaseSensitive(object, key) : NULL;
    if (member == NULL) {
        unread(key, "missing");
    }
    return member;
}

static const char *text_of(const cJSON *object, const char *key) {
    const cJSON *member = spisor_vector_member(object, key);
    if (member != NULL && !cJSON_IsString(member)) {
        unread(key, "not a text");
        return NULL;
    }
    return member == NULL ? NULL : member->valuestring;
}

int spisor_vector_text(const cJSON *object, const char *key, const char **text, size_t *length) {
    const char *found = text_of(object, key);
    if (found == NULL) {
        return 0;
    }
    *text = found;
    *length = strlen(found);
    return 1;
}

int spisor_vector_status(const cJSON *object, const char *key, spisor_status *status) {
    const char *name = text_of(object, key);
    if (name == NULL) {
        return 0;
    }
    for (size_t index = 0; index < sizeof STATUS_NAMES / sizeof STATUS_NAMES[0]; index++) {
        if (strcmp(name, STATUS_NAMES[index].name) == 0) {
            *status = STATUS_NAMES[index].status;
            return 1;
        }
    }
    return unread(key, "names no status");
}

static int optional_text(const cJSON *object, const char *key, const char **text) {
    const cJSON *member = cJSON_GetObjectItemCaseSensitive(object, key);
    if (member == NULL) {
        *text = NULL;
        return 1;
    }
    if (!cJSON_IsString(member)) {
        return unread(key, "not a text");
    }
    *text = member->valuestring;
    return 1;
}

int spisor_vector_refusal(const cJSON *object, const char *key, spisor_refusal *refusal) {
    const cJSON *member = spisor_vector_member(object, key);
    const char *detail = NULL;
    const char *fix = NULL;
    if (member == NULL) {
        return 0;
    }
    if (!cJSON_IsObject(member)) {
        return unread(key, "not a refusal object");
    }
    if (!optional_text(member, "detail", &detail) || !optional_text(member, "fix", &fix)) {
        return 0;
    }
    refusal->detail = detail;
    refusal->fix = fix;
    refusal->storage = NULL;
    return 1;
}

int spisor_vector_value(const cJSON *object, const char *key, spisor_value_tree *tree) {
    const cJSON *member = spisor_vector_member(object, key);
    spisor_status status;
    if (member == NULL) {
        return 0;
    }
    status = spisor_value_of_json(member, tree);
    if (status != SPISOR_OK) {
        return unread(key, status == SPISOR_OUT_OF_MEMORY ? "out of memory" : "not a value tree");
    }
    return 1;
}
