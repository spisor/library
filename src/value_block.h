#ifndef SPISOR_VALUE_BLOCK_H
#define SPISOR_VALUE_BLOCK_H

#include <stddef.h>

#include "spisor/value.h"

/** What one tree needs below its root: every node of its lists, every entry of its records, every text with its NUL. */
typedef struct value_block_size {
    size_t values;
    size_t entries;
    size_t text_bytes;
} value_block_size;

/** Where the next node, entry and text of a block being filled go. */
typedef struct value_block_cursor {
    spisor_value *values;
    spisor_entry *entries;
    char *texts;
} value_block_cursor;

/** Adds to a running count, answering 0 when the sum would not fit a size_t. */
int value_block_add(size_t *total, size_t more);

int value_tree_is_empty(const spisor_value_tree *tree);

/**
 * Allocates one block of the given size, nodes first so every node and entry is aligned, and points the cursor at it.
 * A tree needing nothing below its root needs no block, and `*storage` is NULL.
 *
 * @return OK or OUT_OF_MEMORY, the latter including a size that does not fit a size_t.
 */
spisor_status value_block_allocate(const value_block_size *size, void **storage, value_block_cursor *cursor);

/** Takes `count` nodes from the cursor. */
spisor_value *value_block_take_values(value_block_cursor *cursor, size_t count);

/** Takes `count` entries from the cursor. */
spisor_entry *value_block_take_entries(value_block_cursor *cursor, size_t count);

/** Copies `length` bytes and a NUL to the cursor, answering the copy. */
const char *value_block_copy_text(value_block_cursor *cursor, const char *text, size_t length);

/** Moves a filled root and its block into an empty tree. */
void value_tree_adopt(spisor_value_tree *tree, const spisor_value *root, void *storage);

#endif
