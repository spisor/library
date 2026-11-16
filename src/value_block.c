#include "value_block.h"

#include <stdint.h>
#include <string.h>

#include "allocation.h"

int value_block_add(size_t *total, size_t more) {
    if (more > SIZE_MAX - *total) {
        return 0;
    }
    *total += more;
    return 1;
}

int value_tree_is_empty(const spisor_value_tree *tree) {
    return tree != NULL && tree->size >= sizeof *tree && tree->storage == NULL && tree->root.kind == SPISOR_VALUE_NONE;
}

static int product_fits(size_t count, size_t width, size_t *product) {
    if (count > SIZE_MAX / width) {
        return 0;
    }
    *product = count * width;
    return 1;
}

spisor_status value_block_allocate(const value_block_size *size, void **storage, value_block_cursor *cursor) {
    size_t value_bytes = 0;
    size_t entry_bytes = 0;
    size_t total = 0;
    unsigned char *block;
    if (!product_fits(size->values, sizeof(spisor_value), &value_bytes) ||
        !product_fits(size->entries, sizeof(spisor_entry), &entry_bytes) || !value_block_add(&total, value_bytes) ||
        !value_block_add(&total, entry_bytes) || !value_block_add(&total, size->text_bytes)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    memset(cursor, 0, sizeof *cursor);
    *storage = NULL;
    if (total == 0) {
        return SPISOR_OK;
    }
    block = spisor_memory_allocate(total);
    if (block == NULL) {
        return SPISOR_OUT_OF_MEMORY;
    }
    cursor->values = (spisor_value *)block;
    cursor->entries = (spisor_entry *)(block + value_bytes);
    cursor->texts = (char *)(block + value_bytes + entry_bytes);
    *storage = block;
    return SPISOR_OK;
}

spisor_value *value_block_take_values(value_block_cursor *cursor, size_t count) {
    spisor_value *taken = cursor->values;
    if (count == 0) {
        return NULL;
    }
    cursor->values += count;
    return taken;
}

spisor_entry *value_block_take_entries(value_block_cursor *cursor, size_t count) {
    spisor_entry *taken = cursor->entries;
    if (count == 0) {
        return NULL;
    }
    cursor->entries += count;
    return taken;
}

const char *value_block_copy_text(value_block_cursor *cursor, const char *text, size_t length) {
    char *copy = cursor->texts;
    if (length > 0) {
        memcpy(copy, text, length);
    }
    copy[length] = '\0';
    cursor->texts += length + 1;
    return copy;
}

void value_tree_adopt(spisor_value_tree *tree, const spisor_value *root, void *storage) {
    tree->root = *root;
    tree->storage = storage;
}
