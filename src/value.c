#include "spisor/value.h"

#include <math.h>
#include <string.h>

#include "allocation.h"
#include "utf8.h"
#include "value_block.h"

static int is_text(const char *text, size_t length) {
    return text != NULL && text[length] == '\0' && memchr(text, '\0', length) == NULL &&
           spisor_is_valid_utf8((const uint8_t *)text, length);
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

static int is_same_key(const spisor_entry *left, const spisor_entry *right) {
    return left->key_length == right->key_length && memcmp(left->key, right->key, left->key_length) == 0;
}

static int has_key_twice(const spisor_value *record) {
    for (size_t index = 1; index < record->count; index++) {
        for (size_t earlier = 0; earlier < index; earlier++) {
            if (is_same_key(&record->entries[earlier], &record->entries[index])) {
                return 1;
            }
        }
    }
    return 0;
}

static int is_well_formed(const spisor_value *value, size_t depth);

static int is_well_formed_member(const spisor_value *value, size_t depth) {
    return value->kind != SPISOR_VALUE_NONE && is_well_formed(value, depth);
}

static int is_well_formed(const spisor_value *value, size_t depth) {
    if (depth > SPISOR_VALUE_DEPTH_LIMIT || !holds_only_its_kind(value)) {
        return 0;
    }
    if (value->kind == SPISOR_VALUE_LIST) {
        for (size_t index = 0; index < value->count; index++) {
            if (!is_well_formed_member(&value->items[index], depth + 1)) {
                return 0;
            }
        }
    } else if (value->kind == SPISOR_VALUE_RECORD) {
        for (size_t index = 0; index < value->count; index++) {
            const spisor_entry *entry = &value->entries[index];
            if (!is_text(entry->key, entry->key_length) || !is_well_formed_member(&entry->value, depth + 1)) {
                return 0;
            }
        }
        if (has_key_twice(value)) {
            return 0;
        }
    }
    return 1;
}

spisor_status spisor_value_check(const spisor_value *value) {
    return value != NULL && is_well_formed(value, 0) ? SPISOR_OK : SPISOR_MALFORMED;
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

static int is_equal(const spisor_value *left, const spisor_value *right, size_t depth);

static int are_equal_lists(const spisor_value *left, const spisor_value *right, size_t depth) {
    for (size_t index = 0; index < left->count; index++) {
        if (!is_equal(&left->items[index], &right->items[index], depth + 1)) {
            return 0;
        }
    }
    return 1;
}

static int are_equal_records(const spisor_value *left, const spisor_value *right, size_t depth) {
    for (size_t index = 0; index < left->count; index++) {
        const spisor_entry *entry = &left->entries[index];
        const spisor_value *other = spisor_value_member(right, entry->key, entry->key_length);
        if (other == NULL || !is_equal(&entry->value, other, depth + 1)) {
            return 0;
        }
    }
    return 1;
}

static int is_equal(const spisor_value *left, const spisor_value *right, size_t depth) {
    if (depth > SPISOR_VALUE_DEPTH_LIMIT || left->kind != right->kind) {
        return 0;
    }
    switch (left->kind) {
    case SPISOR_VALUE_BOOLEAN:
        return left->boolean == right->boolean;
    case SPISOR_VALUE_NUMBER:
        return left->number == right->number;
    case SPISOR_VALUE_TEXT:
        return left->length == right->length && memcmp(left->text, right->text, left->length) == 0;
    case SPISOR_VALUE_LIST:
        return left->count == right->count && are_equal_lists(left, right, depth);
    case SPISOR_VALUE_RECORD:
        return left->count == right->count && are_equal_records(left, right, depth);
    default:
        return 1;
    }
}

int spisor_value_equal(const spisor_value *left, const spisor_value *right) {
    return left != NULL && right != NULL && is_equal(left, right, 0);
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
    if (!value_tree_is_empty(tree) || spisor_value_check(source) != SPISOR_OK) {
        return SPISOR_MALFORMED;
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
