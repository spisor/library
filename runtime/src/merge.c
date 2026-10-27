#include "merge.h"

#include <stdlib.h>
#include <string.h>

static cJSON *merge_objects(const cJSON *defaults, const cJSON *file);

static int merge_item(cJSON *result, const cJSON *file_item) {
    cJSON *existing = cJSON_GetObjectItemCaseSensitive(result, file_item->string);
    int both_objects = existing != NULL && cJSON_IsObject(existing) && cJSON_IsObject(file_item);
    cJSON *merged = both_objects ? merge_objects(existing, file_item) : cJSON_Duplicate(file_item, 1);
    if (merged == NULL) {
        return 0;
    }
    if (existing == NULL) {
        return cJSON_AddItemToObject(result, file_item->string, merged);
    }
    return cJSON_ReplaceItemInObjectCaseSensitive(result, file_item->string, merged);
}

static cJSON *merge_objects(const cJSON *defaults, const cJSON *file) {
    cJSON *result = cJSON_Duplicate(defaults, 1);
    if (result == NULL) {
        return NULL;
    }
    for (const cJSON *item = file->child; item != NULL; item = item->next) {
        if (!merge_item(result, item)) {
            cJSON_Delete(result);
            return NULL;
        }
    }
    return result;
}

cJSON *runtime_merge(const cJSON *defaults, const cJSON *file) {
    if (file == NULL) {
        return defaults == NULL ? cJSON_CreateObject() : cJSON_Duplicate(defaults, 1);
    }
    if (defaults == NULL || !cJSON_IsObject(defaults) || !cJSON_IsObject(file)) {
        return cJSON_Duplicate(file, 1);
    }
    return merge_objects(defaults, file);
}

static int canonical_index(const char *segment, int length) {
    if (segment[0] == '\0' || (segment[0] == '0' && segment[1] != '\0')) {
        return -1;
    }
    long index = 0;
    for (const char *digit = segment; *digit != '\0'; digit++) {
        if (*digit < '0' || *digit > '9') {
            return -1;
        }
        index = index * 10 + (*digit - '0');
        if (index >= length) {
            return -1;
        }
    }
    return (int)index;
}

static const cJSON *step(const cJSON *node, const char *segment) {
    if (cJSON_IsObject(node)) {
        return cJSON_GetObjectItemCaseSensitive(node, segment);
    }
    if (cJSON_IsArray(node)) {
        int index = canonical_index(segment, cJSON_GetArraySize(node));
        return index < 0 ? NULL : cJSON_GetArrayItem(node, index);
    }
    return NULL;
}

const cJSON *runtime_walk(const cJSON *tree, const char *key) {
    size_t length = strlen(key);
    char *segment = malloc(length + 1);
    if (segment == NULL) {
        return NULL;
    }
    const cJSON *node = tree;
    const char *start = key;
    for (;;) {
        const char *end = strchr(start, '.');
        size_t segment_length = end != NULL ? (size_t)(end - start) : strlen(start);
        memcpy(segment, start, segment_length);
        segment[segment_length] = '\0';
        node = step(node, segment);
        if (node == NULL || end == NULL) {
            break;
        }
        start = end + 1;
    }
    free(segment);
    return node;
}
