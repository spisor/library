#include "spisor/status.h"

#include <string.h>

#include "allocation.h"

static int is_empty(const spisor_refusal *refusal) {
    return refusal->size >= sizeof *refusal && refusal->storage == NULL;
}

static size_t text_bytes(const char *text) {
    return text == NULL ? 0 : strlen(text) + 1;
}

static const char *copy_text(char **cursor, const char *text) {
    const char *copy = *cursor;
    size_t bytes = text_bytes(text);
    if (text == NULL) {
        return NULL;
    }
    memcpy(*cursor, text, bytes);
    *cursor += bytes;
    return copy;
}

spisor_status spisor_refuse(spisor_refusal *refusal, const char *detail, const char *fix) {
    size_t detail_bytes = text_bytes(detail);
    size_t fix_bytes = text_bytes(fix);
    char *block;
    char *cursor;
    if (refusal == NULL || !is_empty(refusal)) {
        return SPISOR_REFUSED;
    }
    block = spisor_memory_allocate(detail_bytes + fix_bytes + 1);
    if (block == NULL) {
        return SPISOR_OUT_OF_MEMORY;
    }
    cursor = block;
    refusal->detail = copy_text(&cursor, detail);
    refusal->fix = copy_text(&cursor, fix);
    refusal->storage = block;
    return SPISOR_REFUSED;
}

void spisor_refusal_release(spisor_refusal *refusal) {
    if (refusal == NULL || refusal->size < sizeof *refusal) {
        return;
    }
    spisor_memory_release(refusal->storage);
    refusal->detail = NULL;
    refusal->fix = NULL;
    refusal->storage = NULL;
}
