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

#include "spisor/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/** How deep a tree may nest below its root, which also bounds every walk over a tree that points into itself. */
#define SPISOR_VALUE_DEPTH_LIMIT 1000

/** The kinds are append-only: a published kind keeps its number, and none is reused. */
typedef enum spisor_value_kind {
    /** No value at all, as an answer that has none holds: only ever a tree's root, never a member or an item. */
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
 * One node: `kind` says which fields hold it, and every other field is zero. `boolean` is 0 or 1; `number` a finite
 * double; `text` well-formed UTF-8 of `length` bytes, none of them NUL, followed by a NUL; a list `count` nodes at
 * `items`; a record `count` entries at `entries`, in order, each key once. A node sits inside arrays, so it carries no
 * size and never grows: a later kind none of these fields can hold arrives as a new definition.
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

/** One member of a record: a key under the same rules as a text, and its value. */
struct spisor_entry {
    const char *key;
    size_t key_length;
    spisor_value value;
};

/**
 * A tree the library built, owning every node and text under `root` until spisor_value_release. An empty tree has its
 * `size` set, `root` of kind NONE and NULL storage, as SPISOR_INIT leaves it. First shape ends at `storage`.
 */
typedef struct spisor_value_tree {
    uint32_t size;
    spisor_value root;
    void *storage;
} spisor_value_tree;

/**
 * Walks the whole tree and answers whether it keeps every rule of spisor_value and spisor_entry, within
 * SPISOR_VALUE_DEPTH_LIMIT.
 *
 * @return OK; MALFORMED at the first rule broken, and for a NULL value.
 */
spisor_status spisor_value_check(const spisor_value *value);

/**
 * The member of a record under a key, compared byte for byte.
 *
 * @return the member's value; NULL when `record` is not a record or holds no such key.
 */
const spisor_value *spisor_value_member(const spisor_value *record, const char *key, size_t key_length);

/**
 * Whether two well-formed trees hold the same value: the same kinds, booleans, numbers and texts, lists item by item
 * in order, and records with the same keys each holding the same value, in any order, since each key is there once.
 */
int spisor_value_equal(const spisor_value *left, const spisor_value *right);

/**
 * Copies a well-formed tree into an empty one, every node and text in one allocation, so the copy outlives `source`.
 *
 * @param tree the caller's empty tree. Required.
 * @return OK; MALFORMED, with `tree` left as it was, for a source spisor_value_check refuses or a tree that is not
 * empty; OUT_OF_MEMORY with `tree` left as it was.
 */
spisor_status spisor_value_build(const spisor_value *source, spisor_value_tree *tree);

/** Frees what a build filled and leaves the tree empty. NULL, or an empty tree, does nothing. */
void spisor_value_release(spisor_value_tree *tree);

#ifdef __cplusplus
}
#endif

#endif
