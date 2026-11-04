#include "catalog_fold.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "catalog_fold_table.h"

#define HANGUL_FIRST 0xAC00u
#define HANGUL_COUNT 11172u
#define HANGUL_LEADING_FIRST 0x1100u
#define HANGUL_VOWEL_FIRST 0x1161u
#define HANGUL_TRAILING_FIRST 0x11A7u
#define HANGUL_VOWEL_COUNT 21u
#define HANGUL_TRAILING_COUNT 28u
#define LONGEST_FOLD 4u

typedef struct folded_key {
    uint32_t *code_points;
    size_t length;
} folded_key;

static size_t decode_one(const unsigned char *at, uint32_t *code_point) {
    if (at[0] < 0x80) {
        *code_point = at[0];
        return 1;
    }
    if ((at[0] & 0xE0) == 0xC0) {
        *code_point = ((uint32_t)(at[0] & 0x1F) << 6) | (at[1] & 0x3F);
        return 2;
    }
    if ((at[0] & 0xF0) == 0xE0) {
        *code_point = ((uint32_t)(at[0] & 0x0F) << 12) | ((uint32_t)(at[1] & 0x3F) << 6) | (at[2] & 0x3F);
        return 3;
    }
    *code_point = ((uint32_t)(at[0] & 0x07) << 18) | ((uint32_t)(at[1] & 0x3F) << 12) |
                  ((uint32_t)(at[2] & 0x3F) << 6) | (at[3] & 0x3F);
    return 4;
}

static const runtime_catalog_fold_entry *fold_entry_of(uint32_t code_point) {
    size_t low = 0;
    size_t high = runtime_catalog_fold_entry_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        uint32_t found = runtime_catalog_fold_entries[middle].code_point;
        if (found == code_point) {
            return &runtime_catalog_fold_entries[middle];
        }
        if (found < code_point) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return NULL;
}

static uint8_t combining_class_of(uint32_t code_point) {
    size_t low = 0;
    size_t high = runtime_catalog_class_range_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        const runtime_catalog_class_range *range = &runtime_catalog_class_ranges[middle];
        if (code_point < range->first) {
            high = middle;
        } else if (code_point > range->last) {
            low = middle + 1;
        } else {
            return range->combining_class;
        }
    }
    return 0;
}

static size_t fold_one(uint32_t code_point, uint32_t *out) {
    if (code_point >= HANGUL_FIRST && code_point < HANGUL_FIRST + HANGUL_COUNT) {
        uint32_t index = code_point - HANGUL_FIRST;
        uint32_t trailing = index % HANGUL_TRAILING_COUNT;
        out[0] = HANGUL_LEADING_FIRST + index / (HANGUL_VOWEL_COUNT * HANGUL_TRAILING_COUNT);
        out[1] = HANGUL_VOWEL_FIRST + (index % (HANGUL_VOWEL_COUNT * HANGUL_TRAILING_COUNT)) / HANGUL_TRAILING_COUNT;
        out[2] = HANGUL_TRAILING_FIRST + trailing;
        return trailing == 0 ? 2 : 3;
    }
    const runtime_catalog_fold_entry *entry = fold_entry_of(code_point);
    if (entry == NULL) {
        out[0] = code_point;
        return 1;
    }
    memcpy(out, runtime_catalog_fold_pool + entry->start, entry->length * sizeof *out);
    return entry->length;
}

/** Stable insertion sort of each run of combining marks by class, which is canonical ordering. */
static void order_canonically(uint32_t *code_points, size_t length) {
    for (size_t index = 1; index < length; index++) {
        uint32_t moving = code_points[index];
        uint8_t moving_class = combining_class_of(moving);
        if (moving_class == 0) {
            continue;
        }
        size_t at = index;
        while (at > 0) {
            uint8_t before = combining_class_of(code_points[at - 1]);
            if (before == 0 || before <= moving_class) {
                break;
            }
            code_points[at] = code_points[at - 1];
            at--;
        }
        code_points[at] = moving;
    }
}

static int fold_key(const char *key, folded_key *folded) {
    size_t bytes = strlen(key);
    folded->code_points = malloc((bytes * LONGEST_FOLD + 1) * sizeof *folded->code_points);
    folded->length = 0;
    if (folded->code_points == NULL) {
        return 0;
    }
    const unsigned char *at = (const unsigned char *)key;
    while (*at != 0) {
        uint32_t code_point;
        at += decode_one(at, &code_point);
        folded->length += fold_one(code_point, folded->code_points + folded->length);
    }
    order_canonically(folded->code_points, folded->length);
    return 1;
}

static int same_fold(const folded_key *left, const folded_key *right) {
    return left->length == right->length &&
           memcmp(left->code_points, right->code_points, left->length * sizeof *left->code_points) == 0;
}

static void free_folded(folded_key *folded, size_t count) {
    for (size_t index = 0; index < count; index++) {
        free(folded[index].code_points);
    }
    free(folded);
}

int runtime_catalog_folded_duplicate(const char *const *keys, size_t count, size_t *first, size_t *second) {
    folded_key *folded = calloc(count == 0 ? 1 : count, sizeof *folded);
    if (folded == NULL) {
        return -1;
    }
    for (size_t index = 0; index < count; index++) {
        if (!fold_key(keys[index], &folded[index])) {
            free_folded(folded, count);
            return -1;
        }
        for (size_t earlier = 0; earlier < index; earlier++) {
            if (same_fold(&folded[earlier], &folded[index])) {
                *first = earlier;
                *second = index;
                free_folded(folded, count);
                return 1;
            }
        }
    }
    free_folded(folded, count);
    return 0;
}
