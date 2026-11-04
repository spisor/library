#include "catalog_order.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define LARGEST_ARRAY_INDEX 4294967294ull

static int array_index_of(const char *key, uint64_t *index) {
    size_t length = strlen(key);
    if (length == 0 || length > 10 || (length > 1 && key[0] == '0')) {
        return 0;
    }
    uint64_t value = 0;
    for (size_t at = 0; at < length; at++) {
        if (key[at] < '0' || key[at] > '9') {
            return 0;
        }
        value = value * 10 + (uint64_t)(key[at] - '0');
    }
    *index = value;
    return value <= LARGEST_ARRAY_INDEX;
}

static void insert_by_index(const cJSON **members, uint64_t *indexes, size_t filled, const cJSON *member,
                            uint64_t index) {
    size_t at = filled;
    while (at > 0 && indexes[at - 1] > index) {
        members[at] = members[at - 1];
        indexes[at] = indexes[at - 1];
        at--;
    }
    members[at] = member;
    indexes[at] = index;
}

static size_t count_members(const cJSON *object) {
    size_t count = 0;
    for (const cJSON *member = object == NULL ? NULL : object->child; member != NULL; member = member->next) {
        count++;
    }
    return count;
}

const cJSON **runtime_catalog_ordered_members(const cJSON *object, size_t *count) {
    *count = count_members(object);
    if (*count == 0) {
        return NULL;
    }
    const cJSON **members = malloc(*count * sizeof *members);
    uint64_t *indexes = malloc(*count * sizeof *indexes);
    if (members == NULL || indexes == NULL) {
        free(members);
        free(indexes);
        return NULL;
    }
    size_t indexed = 0;
    for (const cJSON *member = object->child; member != NULL; member = member->next) {
        uint64_t index;
        if (array_index_of(member->string, &index)) {
            insert_by_index(members, indexes, indexed++, member, index);
        }
    }
    size_t filled = indexed;
    for (const cJSON *member = object->child; member != NULL; member = member->next) {
        uint64_t index;
        if (!array_index_of(member->string, &index)) {
            members[filled++] = member;
        }
    }
    free(indexes);
    return members;
}
