#ifndef SPISOR_VALUE_H
#define SPISOR_VALUE_H

/**
 * @file
 * One typed value tree, for data a library only stores or hands back and whose shape belongs to someone else: a
 * setting's value, a catalog item, ui props. A field whose shape the library decides is typed by its own struct, never
 * by this.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The kinds are append-only: a published kind keeps its number, and none is reused. */
typedef enum spisor_value_kind {
    /** No value at all, as an answer that has none holds; never a value to store. */
    SPISOR_VALUE_NONE = 0,
    SPISOR_VALUE_NULL = 1,
    SPISOR_VALUE_BOOLEAN = 2,
    SPISOR_VALUE_NUMBER = 3,
    SPISOR_VALUE_TEXT = 4,
    SPISOR_VALUE_LIST = 5,
    SPISOR_VALUE_RECORD = 6
} spisor_value_kind;

typedef struct spisor_entry spisor_entry;

/**
 * One node: `kind` says which fields hold it and every other field is zero. `boolean` is 0 or 1; `number` a finite
 * double; `text` UTF-8 of `length` bytes followed by a NUL; a list `count` nodes at `items`; a record `count` entries
 * at `entries`, in order, each key once. A node sits inside arrays, so it carries no size and never grows.
 */
typedef struct spisor_value {
    uint32_t kind;
    uint32_t boolean;
    double number;
    const char *text;
    size_t length;
    const struct spisor_value *items;
    const spisor_entry *entries;
    size_t count;
} spisor_value;

/** One member of a record: a UTF-8 key of `key_length` bytes followed by a NUL, and its value. */
struct spisor_entry {
    const char *key;
    size_t key_length;
    spisor_value value;
};

#ifdef __cplusplus
}
#endif

#endif
