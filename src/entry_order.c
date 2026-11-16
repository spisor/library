#include "entry_order.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "allocation.h"

static int compare_keys(const void *left, const void *right) {
    const spisor_entry *first = *(const spisor_entry *const *)left;
    const spisor_entry *second = *(const spisor_entry *const *)right;
    size_t shorter = first->key_length < second->key_length ? first->key_length : second->key_length;
    int order = memcmp(first->key, second->key, shorter);
    if (order != 0) {
        return order;
    }
    return (first->key_length > second->key_length) - (first->key_length < second->key_length);
}

spisor_status spisor_entries_by_key(const spisor_value *record, const spisor_entry ***sorted) {
    const spisor_entry **entries;
    *sorted = NULL;
    if (record->count < 2) {
        return SPISOR_OK;
    }
    if (record->count > SIZE_MAX / sizeof *entries) {
        return SPISOR_OUT_OF_MEMORY;
    }
    entries = spisor_memory_allocate(record->count * sizeof *entries);
    if (entries == NULL) {
        return SPISOR_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < record->count; index++) {
        entries[index] = &record->entries[index];
    }
    qsort((void *)entries, record->count, sizeof *entries, compare_keys);
    *sorted = entries;
    return SPISOR_OK;
}

int spisor_entries_share_a_key(const spisor_entry *left, const spisor_entry *right) {
    return left->key_length == right->key_length && memcmp(left->key, right->key, left->key_length) == 0;
}
