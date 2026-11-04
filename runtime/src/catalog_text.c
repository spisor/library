#include "catalog_text.h"

#include <stdlib.h>
#include <string.h>

#define FIRST_CAPACITY 64

void runtime_catalog_text_start(runtime_catalog_text *text) {
    text->bytes = NULL;
    text->length = 0;
    text->capacity = 0;
    text->failed = 0;
}

static int reserve(runtime_catalog_text *text, size_t more) {
    if (text->failed) {
        return 0;
    }
    if (text->length + more + 1 <= text->capacity) {
        return 1;
    }
    size_t capacity = text->capacity == 0 ? FIRST_CAPACITY : text->capacity;
    while (capacity < text->length + more + 1) {
        capacity *= 2;
    }
    char *grown = realloc(text->bytes, capacity);
    if (grown == NULL) {
        text->failed = 1;
        return 0;
    }
    text->bytes = grown;
    text->capacity = capacity;
    return 1;
}

int runtime_catalog_text_add_bytes(runtime_catalog_text *text, const char *bytes, size_t length) {
    if (!reserve(text, length)) {
        return 0;
    }
    if (length > 0) {
        memcpy(text->bytes + text->length, bytes, length);
    }
    text->length += length;
    text->bytes[text->length] = 0;
    return 1;
}

int runtime_catalog_text_add(runtime_catalog_text *text, const char *string) {
    return runtime_catalog_text_add_bytes(text, string, strlen(string));
}

int runtime_catalog_text_add_character(runtime_catalog_text *text, char character) {
    return runtime_catalog_text_add_bytes(text, &character, 1);
}

int runtime_catalog_text_add_escaped_byte(runtime_catalog_text *text, unsigned char byte) {
    static const char digits[] = "0123456789ABCDEF";
    char escaped[3] = {'%', digits[byte >> 4], digits[byte & 15]};
    return runtime_catalog_text_add_bytes(text, escaped, sizeof escaped);
}

void runtime_catalog_text_cut(runtime_catalog_text *text, size_t length) {
    if (text->bytes != NULL && length < text->length) {
        text->length = length;
        text->bytes[length] = 0;
    }
}

char *runtime_catalog_text_finish(runtime_catalog_text *text) {
    if (text->failed || !reserve(text, 0)) {
        runtime_catalog_text_discard(text);
        return NULL;
    }
    text->bytes[text->length] = 0;
    char *finished = text->bytes;
    runtime_catalog_text_start(text);
    return finished;
}

void runtime_catalog_text_discard(runtime_catalog_text *text) {
    free(text->bytes);
    runtime_catalog_text_start(text);
}

char *runtime_catalog_copy_bytes(const char *bytes, size_t length) {
    char *copy = malloc(length + 1);
    if (copy != NULL) {
        memcpy(copy, bytes, length);
        copy[length] = 0;
    }
    return copy;
}

char *runtime_catalog_copy(const char *string) {
    return runtime_catalog_copy_bytes(string, strlen(string));
}
