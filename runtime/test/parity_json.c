#include "parity_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cJSON *json_of_list(const spisor_value *value) {
    cJSON *list = cJSON_CreateArray();
    for (size_t index = 0; list != NULL && index < value->count; index++) {
        if (!cJSON_AddItemToArray(list, parity_json_of_value(&value->items[index]))) {
            cJSON_Delete(list);
            list = NULL;
        }
    }
    return list;
}

static cJSON *json_of_record(const spisor_value *value) {
    cJSON *record = cJSON_CreateObject();
    for (size_t index = 0; record != NULL && index < value->count; index++) {
        const spisor_entry *entry = &value->entries[index];
        if (!cJSON_AddItemToObject(record, entry->key, parity_json_of_value(&entry->value))) {
            cJSON_Delete(record);
            record = NULL;
        }
    }
    return record;
}

cJSON *parity_json_of_value(const spisor_value *value) {
    switch (value->kind) {
    case SPISOR_VALUE_NULL:
        return cJSON_CreateNull();
    case SPISOR_VALUE_BOOLEAN:
        return cJSON_CreateBool(value->boolean != 0);
    case SPISOR_VALUE_NUMBER:
        return cJSON_CreateNumber(value->number);
    case SPISOR_VALUE_TEXT:
        return cJSON_CreateString(value->text);
    case SPISOR_VALUE_LIST:
        return json_of_list(value);
    case SPISOR_VALUE_RECORD:
        return json_of_record(value);
    default:
        return NULL;
    }
}

static void slash_backslashes(char *text) {
    for (char *character = text; *character != '\0'; character++) {
        if (*character == '\\') {
            *character = '/';
        }
    }
}

static char *slashed_copy(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy != NULL) {
        memcpy(copy, text, size);
        slash_backslashes(copy);
    }
    return copy;
}

static size_t occurrences(const char *text, const char *needle, size_t needle_length) {
    size_t count = 0;
    for (const char *found = strstr(text, needle); found != NULL; found = strstr(found + needle_length, needle)) {
        count++;
    }
    return count;
}

/** `text` with every `needle` written as `token`, allocated with malloc; NULL when memory runs out. */
static char *replaced(const char *text, const char *needle, const char *token) {
    size_t needle_length = strlen(needle);
    size_t token_length = strlen(token);
    size_t count = needle_length == 0 ? 0 : occurrences(text, needle, needle_length);
    char *result = malloc(strlen(text) + count * token_length + 1);
    char *written = result;
    const char *rest = text;
    for (size_t index = 0; result != NULL && index < count; index++) {
        const char *found = strstr(rest, needle);
        memcpy(written, rest, (size_t)(found - rest));
        written += found - rest;
        memcpy(written, token, token_length);
        written += token_length;
        rest = found + needle_length;
    }
    if (result != NULL) {
        memcpy(written, rest, strlen(rest) + 1);
    }
    return result;
}

static char *normalised_text(const char *text, const char *home, const char *root) {
    char *slashed = slashed_copy(text);
    char *without_home = slashed == NULL ? NULL : replaced(slashed, home, "{home}");
    char *without_root = without_home == NULL ? NULL : replaced(without_home, root, "{root}");
    free(without_home);
    free(slashed);
    return without_root;
}

static int normalise_with(cJSON *json, const char *home, const char *root) {
    if (cJSON_IsString(json)) {
        char *text = normalised_text(json->valuestring, home, root);
        int set = text != NULL && cJSON_SetValuestring(json, text) != NULL;
        free(text);
        return set;
    }
    int normalised = 1;
    for (cJSON *child = json->child; normalised && child != NULL; child = child->next) {
        normalised = normalise_with(child, home, root);
    }
    return normalised;
}

int parity_normalise(cJSON *json, const parity_tokens *tokens) {
    char *home = slashed_copy(tokens->home);
    char *root = slashed_copy(tokens->root);
    int normalised = home != NULL && root != NULL && normalise_with(json, home, root);
    free(home);
    free(root);
    return normalised;
}

static int is_same_scalar(const cJSON *observed, const cJSON *expected) {
    if ((observed->type & 0xFF) != (expected->type & 0xFF)) {
        return 0;
    }
    if (cJSON_IsNumber(observed)) {
        return observed->valuedouble == expected->valuedouble;
    }
    if (cJSON_IsString(observed)) {
        return strcmp(observed->valuestring, expected->valuestring) == 0;
    }
    return 1;
}

static int differs_at(const cJSON *observed, const cJSON *expected, parity_difference *difference);

/** Appends one path segment, runs the comparison below it, and takes the segment back off when nothing differs. */
static int differs_below(const cJSON *observed, const cJSON *expected, const char *segment,
                         parity_difference *difference) {
    size_t length = strlen(difference->path);
    snprintf(difference->path + length, sizeof difference->path - length, "%s", segment);
    if (differs_at(observed, expected, difference)) {
        return 1;
    }
    difference->path[length] = '\0';
    return 0;
}

static int list_differs(const cJSON *observed, const cJSON *expected, parity_difference *difference) {
    int observed_count = cJSON_GetArraySize(observed);
    int expected_count = cJSON_GetArraySize(expected);
    int longest = observed_count > expected_count ? observed_count : expected_count;
    for (int index = 0; index < longest; index++) {
        char segment[32];
        snprintf(segment, sizeof segment, "[%d]", index);
        if (differs_below(cJSON_GetArrayItem(observed, index), cJSON_GetArrayItem(expected, index), segment,
                          difference)) {
            return 1;
        }
    }
    return 0;
}

static int member_differs(const cJSON *observed, const cJSON *expected, const char *key,
                          parity_difference *difference) {
    char segment[512];
    snprintf(segment, sizeof segment, "[\"%s\"]", key);
    return differs_below(cJSON_GetObjectItemCaseSensitive(observed, key),
                         cJSON_GetObjectItemCaseSensitive(expected, key), segment, difference);
}

static int record_differs(const cJSON *observed, const cJSON *expected, parity_difference *difference) {
    for (const cJSON *member = expected->child; member != NULL; member = member->next) {
        if (member_differs(observed, expected, member->string, difference)) {
            return 1;
        }
    }
    for (const cJSON *member = observed->child; member != NULL; member = member->next) {
        if (cJSON_GetObjectItemCaseSensitive(expected, member->string) == NULL &&
            member_differs(observed, expected, member->string, difference)) {
            return 1;
        }
    }
    return 0;
}

static int differs_at(const cJSON *observed, const cJSON *expected, parity_difference *difference) {
    difference->observed = observed;
    difference->expected = expected;
    if (observed == NULL || expected == NULL) {
        return observed != expected;
    }
    if (cJSON_IsArray(observed) && cJSON_IsArray(expected)) {
        return list_differs(observed, expected, difference);
    }
    if (cJSON_IsObject(observed) && cJSON_IsObject(expected)) {
        return record_differs(observed, expected, difference);
    }
    return !is_same_scalar(observed, expected);
}

int parity_first_difference(const cJSON *observed, const cJSON *expected, parity_difference *difference) {
    difference->path[0] = '\0';
    return differs_at(observed, expected, difference);
}
