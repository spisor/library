#include "spisor/value.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "allocation.h"
#include "entry_order.h"
#include "naula/json.h"
#include "value_block.h"

static int is_text(const char *text, size_t length) {
    return text != NULL && text[length] == '\0' && memchr(text, '\0', length) == NULL &&
           naula_json_is_valid_utf8((const uint8_t *)text, length);
}

static int holds_no_text(const spisor_value *value) {
    return value->text == NULL && value->length == 0;
}

static int holds_no_children(const spisor_value *value) {
    return value->items == NULL && value->entries == NULL && value->count == 0;
}

static int holds_only_its_kind(const spisor_value *value) {
    int no_boolean = value->boolean == 0;
    int no_number = value->number == 0.0;
    switch (value->kind) {
    case SPISOR_VALUE_NONE:
    case SPISOR_VALUE_NULL:
        return no_boolean && no_number && holds_no_text(value) && holds_no_children(value);
    case SPISOR_VALUE_BOOLEAN:
        return value->boolean <= 1 && no_number && holds_no_text(value) && holds_no_children(value);
    case SPISOR_VALUE_NUMBER:
        return no_boolean && isfinite(value->number) && holds_no_text(value) && holds_no_children(value);
    case SPISOR_VALUE_TEXT:
        return no_boolean && no_number && is_text(value->text, value->length) && holds_no_children(value);
    case SPISOR_VALUE_LIST:
        return no_boolean && no_number && holds_no_text(value) && value->entries == NULL &&
               (value->count == 0 || value->items != NULL);
    case SPISOR_VALUE_RECORD:
        return no_boolean && no_number && holds_no_text(value) && value->items == NULL &&
               (value->count == 0 || value->entries != NULL);
    default:
        return 0;
    }
}

typedef struct reached_nodes {
    const spisor_value **nodes;
    size_t count;
    size_t capacity;
} reached_nodes;

#define FIRST_REACHED_CAPACITY 64

static int compare_addresses(const void *left, const void *right) {
    uintptr_t first = (uintptr_t)*(const spisor_value *const *)left;
    uintptr_t second = (uintptr_t)*(const spisor_value *const *)right;
    return (first > second) - (first < second);
}

static int holds_a_node_twice(reached_nodes *reached) {
    if (reached->count < 2) {
        return 0;
    }
    qsort((void *)reached->nodes, reached->count, sizeof *reached->nodes, compare_addresses);
    for (size_t index = 1; index < reached->count; index++) {
        if (reached->nodes[index] == reached->nodes[index - 1]) {
            return 1;
        }
    }
    return 0;
}

static spisor_status grow(reached_nodes *reached) {
    size_t capacity = reached->capacity == 0 ? FIRST_REACHED_CAPACITY : reached->capacity * 2;
    const spisor_value **nodes;
    if (capacity > SIZE_MAX / sizeof *nodes) {
        return SPISOR_OUT_OF_MEMORY;
    }
    nodes = spisor_memory_allocate(capacity * sizeof *nodes);
    if (nodes == NULL) {
        return SPISOR_OUT_OF_MEMORY;
    }
    if (reached->count > 0) {
        memcpy((void *)nodes, (const void *)reached->nodes, reached->count * sizeof *nodes);
    }
    spisor_memory_release((void *)reached->nodes);
    reached->nodes = nodes;
    reached->capacity = capacity;
    return SPISOR_OK;
}

/**
 * @implNote The reached nodes are searched for a repeat each time their array fills, before it doubles, so a shared
 * subtree is refused once at most twice as many nodes as the tree holds have been walked, never after every path.
 */
static spisor_status reach(reached_nodes *reached, const spisor_value *node) {
    if (reached->count == reached->capacity) {
        spisor_status status;
        if (holds_a_node_twice(reached)) {
            return SPISOR_MALFORMED;
        }
        status = grow(reached);
        if (status != SPISOR_OK) {
            return status;
        }
    }
    reached->nodes[reached->count++] = node;
    return SPISOR_OK;
}

static spisor_status refuse_a_key_held_twice(const spisor_value *record) {
    const spisor_entry **sorted = NULL;
    spisor_status status = spisor_entries_by_key(record, &sorted);
    if (status != SPISOR_OK || sorted == NULL) {
        return status;
    }
    for (size_t index = 1; index < record->count; index++) {
        if (spisor_entries_share_a_key(sorted[index - 1], sorted[index])) {
            status = SPISOR_MALFORMED;
            break;
        }
    }
    spisor_memory_release((void *)sorted);
    return status;
}

static spisor_status walk(const spisor_value *value, size_t depth, reached_nodes *reached);

static spisor_status walk_member(const spisor_value *value, size_t depth, reached_nodes *reached) {
    spisor_status status;
    if (value->kind == SPISOR_VALUE_NONE) {
        return SPISOR_MALFORMED;
    }
    status = reach(reached, value);
    return status == SPISOR_OK ? walk(value, depth, reached) : status;
}

static spisor_status walk(const spisor_value *value, size_t depth, reached_nodes *reached) {
    spisor_status status = SPISOR_OK;
    if (depth > SPISOR_VALUE_DEPTH_LIMIT || !holds_only_its_kind(value)) {
        return SPISOR_MALFORMED;
    }
    if (value->kind == SPISOR_VALUE_LIST) {
        for (size_t index = 0; status == SPISOR_OK && index < value->count; index++) {
            status = walk_member(&value->items[index], depth + 1, reached);
        }
    } else if (value->kind == SPISOR_VALUE_RECORD) {
        for (size_t index = 0; status == SPISOR_OK && index < value->count; index++) {
            const spisor_entry *entry = &value->entries[index];
            status = is_text(entry->key, entry->key_length) ? walk_member(&entry->value, depth + 1, reached)
                                                              : SPISOR_MALFORMED;
        }
        if (status == SPISOR_OK) {
            status = refuse_a_key_held_twice(value);
        }
    }
    return status;
}

spisor_status spisor_value_check(const spisor_value *value) {
    reached_nodes reached = {NULL, 0, 0};
    spisor_status status;
    if (value == NULL) {
        return SPISOR_MALFORMED;
    }
    status = walk(value, 0, &reached);
    if (status == SPISOR_OK && holds_a_node_twice(&reached)) {
        status = SPISOR_MALFORMED;
    }
    spisor_memory_release((void *)reached.nodes);
    return status;
}

const spisor_value *spisor_value_member(const spisor_value *record, const char *key, size_t key_length) {
    if (record == NULL || record->kind != SPISOR_VALUE_RECORD || key == NULL) {
        return NULL;
    }
    for (size_t index = 0; index < record->count; index++) {
        const spisor_entry *entry = &record->entries[index];
        if (entry->key_length == key_length && memcmp(entry->key, key, key_length) == 0) {
            return &entry->value;
        }
    }
    return NULL;
}

static int add_text(value_block_size *size, size_t length) {
    return value_block_add(&size->text_bytes, length) && value_block_add(&size->text_bytes, 1);
}

static int measure(const spisor_value *value, value_block_size *size) {
    if (value->kind == SPISOR_VALUE_TEXT) {
        return add_text(size, value->length);
    }
    if (value->kind == SPISOR_VALUE_LIST) {
        if (!value_block_add(&size->values, value->count)) {
            return 0;
        }
        for (size_t index = 0; index < value->count; index++) {
            if (!measure(&value->items[index], size)) {
                return 0;
            }
        }
    } else if (value->kind == SPISOR_VALUE_RECORD) {
        if (!value_block_add(&size->entries, value->count)) {
            return 0;
        }
        for (size_t index = 0; index < value->count; index++) {
            if (!add_text(size, value->entries[index].key_length) || !measure(&value->entries[index].value, size)) {
                return 0;
            }
        }
    }
    return 1;
}

static void copy_node(const spisor_value *source, spisor_value *copy, value_block_cursor *cursor) {
    memset(copy, 0, sizeof *copy);
    copy->kind = source->kind;
    copy->boolean = source->boolean;
    copy->number = source->number;
    copy->count = source->count;
    if (source->kind == SPISOR_VALUE_TEXT) {
        copy->text = value_block_copy_text(cursor, source->text, source->length);
        copy->length = source->length;
    } else if (source->kind == SPISOR_VALUE_LIST) {
        spisor_value *items = value_block_take_values(cursor, source->count);
        copy->items = items;
        for (size_t index = 0; index < source->count; index++) {
            copy_node(&source->items[index], &items[index], cursor);
        }
    } else if (source->kind == SPISOR_VALUE_RECORD) {
        spisor_entry *entries = value_block_take_entries(cursor, source->count);
        copy->entries = entries;
        for (size_t index = 0; index < source->count; index++) {
            const spisor_entry *entry = &source->entries[index];
            entries[index].key = value_block_copy_text(cursor, entry->key, entry->key_length);
            entries[index].key_length = entry->key_length;
            copy_node(&entry->value, &entries[index].value, cursor);
        }
    }
}

spisor_status spisor_value_build(const spisor_value *source, spisor_value_tree *tree) {
    value_block_size size = {0, 0, 0};
    value_block_cursor cursor;
    void *storage = NULL;
    spisor_value root;
    spisor_status status;
    if (!value_tree_is_empty(tree) || source == NULL) {
        return SPISOR_MALFORMED;
    }
    status = spisor_value_check(source);
    if (status != SPISOR_OK) {
        return status;
    }
    if (!measure(source, &size)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    status = value_block_allocate(&size, &storage, &cursor);
    if (status != SPISOR_OK) {
        return status;
    }
    copy_node(source, &root, &cursor);
    value_tree_adopt(tree, &root, storage);
    return SPISOR_OK;
}

void spisor_value_release(spisor_value_tree *tree) {
    if (tree == NULL || tree->size < sizeof *tree) {
        return;
    }
    spisor_memory_release(tree->storage);
    tree->storage = NULL;
    memset(&tree->root, 0, sizeof tree->root);
}
