#include "value_equal.h"

#include <string.h>

#include "allocation.h"
#include "entry_order.h"

static int is_equal(const spisor_value *left, const spisor_value *right, size_t depth);

static int are_equal_lists(const spisor_value *left, const spisor_value *right, size_t depth) {
    for (size_t index = 0; index < left->count; index++) {
        if (!is_equal(&left->items[index], &right->items[index], depth + 1)) {
            return 0;
        }
    }
    return 1;
}

static int are_equal_sorted(const spisor_entry **left, const spisor_entry **right, size_t count, size_t depth) {
    for (size_t index = 0; index < count; index++) {
        if (!spisor_entries_share_a_key(left[index], right[index]) ||
            !is_equal(&left[index]->value, &right[index]->value, depth + 1)) {
            return 0;
        }
    }
    return 1;
}

static int are_equal_records(const spisor_value *left, const spisor_value *right, size_t depth) {
    const spisor_entry **left_sorted = NULL;
    const spisor_entry **right_sorted = NULL;
    int equal = 0;
    if (left->count == 1) {
        return spisor_entries_share_a_key(&left->entries[0], &right->entries[0]) &&
               is_equal(&left->entries[0].value, &right->entries[0].value, depth + 1);
    }
    if (spisor_entries_by_key(left, &left_sorted) == SPISOR_OK &&
        spisor_entries_by_key(right, &right_sorted) == SPISOR_OK) {
        equal = left->count == 0 || are_equal_sorted(left_sorted, right_sorted, left->count, depth);
    }
    spisor_memory_release((void *)left_sorted);
    spisor_memory_release((void *)right_sorted);
    return equal;
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
