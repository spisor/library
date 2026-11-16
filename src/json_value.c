#include "json_value.h"

#include <string.h>

#include "allocation.h"
#include "value_block.h"

typedef enum measure_outcome {
    MEASURED,
    MEASURE_MALFORMED,
    MEASURE_TOO_LARGE
} measure_outcome;

static measure_outcome add_text(value_block_size *size, const char *text) {
    if (text == NULL) {
        return MEASURE_MALFORMED;
    }
    return value_block_add(&size->text_bytes, strlen(text)) && value_block_add(&size->text_bytes, 1)
               ? MEASURED
               : MEASURE_TOO_LARGE;
}

static measure_outcome measure(const cJSON *json, size_t depth, value_block_size *size) {
    const cJSON *child = NULL;
    if (json == NULL || depth > SPISOR_VALUE_DEPTH_LIMIT) {
        return MEASURE_MALFORMED;
    }
    if (cJSON_IsNull(json) || cJSON_IsBool(json) || cJSON_IsNumber(json)) {
        return MEASURED;
    }
    if (cJSON_IsString(json)) {
        return add_text(size, json->valuestring);
    }
    if (!cJSON_IsArray(json) && !cJSON_IsObject(json)) {
        return MEASURE_MALFORMED;
    }
    cJSON_ArrayForEach(child, json) {
        measure_outcome outcome = cJSON_IsObject(json) ? add_text(size, child->string) : MEASURED;
        if (outcome == MEASURED) {
            outcome = value_block_add(cJSON_IsObject(json) ? &size->entries : &size->values, 1)
                          ? measure(child, depth + 1, size)
                          : MEASURE_TOO_LARGE;
        }
        if (outcome != MEASURED) {
            return outcome;
        }
    }
    return MEASURED;
}

static size_t child_count(const cJSON *json) {
    size_t count = 0;
    const cJSON *child = NULL;
    cJSON_ArrayForEach(child, json) {
        count++;
    }
    return count;
}

static void fill(const cJSON *json, spisor_value *value, value_block_cursor *cursor) {
    const cJSON *child = NULL;
    size_t index = 0;
    memset(value, 0, sizeof *value);
    if (cJSON_IsNull(json)) {
        value->kind = SPISOR_VALUE_NULL;
    } else if (cJSON_IsBool(json)) {
        value->kind = SPISOR_VALUE_BOOLEAN;
        value->boolean = cJSON_IsTrue(json) ? 1 : 0;
    } else if (cJSON_IsNumber(json)) {
        value->kind = SPISOR_VALUE_NUMBER;
        value->number = json->valuedouble;
    } else if (cJSON_IsString(json)) {
        value->kind = SPISOR_VALUE_TEXT;
        value->length = strlen(json->valuestring);
        value->text = value_block_copy_text(cursor, json->valuestring, value->length);
    } else if (cJSON_IsArray(json)) {
        spisor_value *items = value_block_take_values(cursor, child_count(json));
        value->kind = SPISOR_VALUE_LIST;
        value->items = items;
        cJSON_ArrayForEach(child, json) {
            fill(child, &items[index++], cursor);
        }
        value->count = index;
    } else {
        spisor_entry *entries = value_block_take_entries(cursor, child_count(json));
        value->kind = SPISOR_VALUE_RECORD;
        value->entries = entries;
        cJSON_ArrayForEach(child, json) {
            entries[index].key_length = strlen(child->string);
            entries[index].key = value_block_copy_text(cursor, child->string, entries[index].key_length);
            fill(child, &entries[index].value, cursor);
            index++;
        }
        value->count = index;
    }
}

spisor_status spisor_value_of_json(const cJSON *json, spisor_value_tree *tree) {
    value_block_size size = {0, 0, 0};
    value_block_cursor cursor;
    void *storage = NULL;
    spisor_value root;
    spisor_status status;
    measure_outcome outcome;
    if (!value_tree_is_empty(tree)) {
        return SPISOR_MALFORMED;
    }
    outcome = measure(json, 0, &size);
    if (outcome != MEASURED) {
        return outcome == MEASURE_MALFORMED ? SPISOR_MALFORMED : SPISOR_OUT_OF_MEMORY;
    }
    status = value_block_allocate(&size, &storage, &cursor);
    if (status != SPISOR_OK) {
        return status;
    }
    fill(json, &root, &cursor);
    status = spisor_value_check(&root);
    if (status != SPISOR_OK) {
        spisor_memory_release(storage);
        return status;
    }
    value_tree_adopt(tree, &root, storage);
    return SPISOR_OK;
}
