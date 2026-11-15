#include "value_tree.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct node_counts {
    size_t values;
    size_t entries;
} node_counts;

typedef struct node_cursor {
    spisor_value *values;
    spisor_entry *entries;
} node_cursor;

static int add_member(cJSON *record, const spisor_entry *entry, int *malformed) {
    if (entry->key == NULL) {
        *malformed = 1;
        return 0;
    }
    cJSON *member = runtime_tree_of_value(&entry->value, malformed);
    if (member == NULL) {
        return 0;
    }
    if (!cJSON_AddItemToObject(record, entry->key, member)) {
        cJSON_Delete(member);
        return 0;
    }
    return 1;
}

static cJSON *list_tree(const spisor_value *value, int *malformed) {
    if (value->count > 0 && value->items == NULL) {
        *malformed = 1;
        return NULL;
    }
    cJSON *list = cJSON_CreateArray();
    for (size_t index = 0; list != NULL && index < value->count; index++) {
        cJSON *item = runtime_tree_of_value(&value->items[index], malformed);
        if (item == NULL || !cJSON_AddItemToArray(list, item)) {
            cJSON_Delete(item);
            cJSON_Delete(list);
            return NULL;
        }
    }
    return list;
}

static cJSON *record_tree(const spisor_value *value, int *malformed) {
    if (value->count > 0 && value->entries == NULL) {
        *malformed = 1;
        return NULL;
    }
    cJSON *record = cJSON_CreateObject();
    for (size_t index = 0; record != NULL && index < value->count; index++) {
        if (!add_member(record, &value->entries[index], malformed)) {
            cJSON_Delete(record);
            return NULL;
        }
    }
    return record;
}

cJSON *runtime_tree_of_value(const spisor_value *value, int *malformed) {
    *malformed = 0;
    switch (value->kind) {
    case SPISOR_VALUE_NULL:
        return cJSON_CreateNull();
    case SPISOR_VALUE_BOOLEAN:
        return cJSON_CreateBool(value->boolean != 0);
    case SPISOR_VALUE_NUMBER:
        if (!isfinite(value->number)) {
            *malformed = 1;
            return NULL;
        }
        return cJSON_CreateNumber(value->number);
    case SPISOR_VALUE_TEXT:
        if (value->text == NULL) {
            *malformed = 1;
            return NULL;
        }
        return cJSON_CreateString(value->text);
    case SPISOR_VALUE_LIST:
        return list_tree(value, malformed);
    case SPISOR_VALUE_RECORD:
        return record_tree(value, malformed);
    default:
        *malformed = 1;
        return NULL;
    }
}

static void count_nodes(const cJSON *tree, node_counts *counts) {
    const cJSON *child = NULL;
    if (cJSON_IsArray(tree)) {
        cJSON_ArrayForEach(child, tree) {
            counts->values++;
            count_nodes(child, counts);
        }
    } else if (cJSON_IsObject(tree)) {
        cJSON_ArrayForEach(child, tree) {
            counts->entries++;
            count_nodes(child, counts);
        }
    }
}

static void fill_node(const cJSON *tree, spisor_value *value, node_cursor *cursor) {
    memset(value, 0, sizeof *value);
    const cJSON *child = NULL;
    if (cJSON_IsNull(tree)) {
        value->kind = SPISOR_VALUE_NULL;
    } else if (cJSON_IsBool(tree)) {
        value->kind = SPISOR_VALUE_BOOLEAN;
        value->boolean = cJSON_IsTrue(tree) ? 1 : 0;
    } else if (cJSON_IsNumber(tree)) {
        value->kind = SPISOR_VALUE_NUMBER;
        value->number = tree->valuedouble;
    } else if (cJSON_IsString(tree)) {
        value->kind = SPISOR_VALUE_TEXT;
        value->text = tree->valuestring;
        value->length = strlen(tree->valuestring);
    } else if (cJSON_IsArray(tree)) {
        spisor_value *items = cursor->values;
        value->kind = SPISOR_VALUE_LIST;
        value->items = items;
        value->count = (size_t)cJSON_GetArraySize(tree);
        cursor->values += value->count;
        size_t index = 0;
        cJSON_ArrayForEach(child, tree) {
            fill_node(child, &items[index++], cursor);
        }
    } else if (cJSON_IsObject(tree)) {
        spisor_entry *entries = cursor->entries;
        value->kind = SPISOR_VALUE_RECORD;
        value->entries = entries;
        value->count = (size_t)cJSON_GetArraySize(tree);
        cursor->entries += value->count;
        size_t index = 0;
        cJSON_ArrayForEach(child, tree) {
            entries[index].key = child->string;
            entries[index].key_length = strlen(child->string);
            fill_node(child, &entries[index].value, cursor);
            index++;
        }
    }
}

runtime_value_storage *runtime_value_of_tree(cJSON *tree, spisor_value *value) {
    node_counts counts = {0, 0};
    count_nodes(tree, &counts);
    runtime_value_storage *storage = malloc(sizeof *storage);
    void *nodes = malloc(counts.values * sizeof(spisor_value) + counts.entries * sizeof(spisor_entry) + 1);
    if (storage == NULL || nodes == NULL) {
        free(storage);
        free(nodes);
        cJSON_Delete(tree);
        return NULL;
    }
    storage->tree = tree;
    storage->nodes = nodes;
    node_cursor cursor = {(spisor_value *)nodes, (spisor_entry *)((spisor_value *)nodes + counts.values)};
    fill_node(tree, value, &cursor);
    return storage;
}

void runtime_value_storage_free(runtime_value_storage *storage) {
    if (storage == NULL) {
        return;
    }
    cJSON_Delete(storage->tree);
    free(storage->nodes);
    free(storage);
}
