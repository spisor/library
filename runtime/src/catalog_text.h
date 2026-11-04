#ifndef RUNTIME_CATALOG_TEXT_H
#define RUNTIME_CATALOG_TEXT_H

#include <stddef.h>

/**
 * A growing NUL-terminated string. Every append answers 0 once memory has run out and keeps answering
 * 0, so a caller checks the last one, or failed, rather than every one.
 */
typedef struct runtime_catalog_text {
    char *bytes;
    size_t length;
    size_t capacity;
    int failed;
} runtime_catalog_text;

void runtime_catalog_text_start(runtime_catalog_text *text);

int runtime_catalog_text_add_bytes(runtime_catalog_text *text, const char *bytes, size_t length);

int runtime_catalog_text_add(runtime_catalog_text *text, const char *string);

int runtime_catalog_text_add_character(runtime_catalog_text *text, char character);

/** Appends the byte as %XX with uppercase hex. */
int runtime_catalog_text_add_escaped_byte(runtime_catalog_text *text, unsigned char byte);

void runtime_catalog_text_cut(runtime_catalog_text *text, size_t length);

/** Hands over the string, or NULL when memory ran out; the text is empty again afterwards. */
char *runtime_catalog_text_finish(runtime_catalog_text *text);

void runtime_catalog_text_discard(runtime_catalog_text *text);

/** A copy of length bytes of bytes, NUL-terminated, freed with free. */
char *runtime_catalog_copy_bytes(const char *bytes, size_t length);

char *runtime_catalog_copy(const char *string);

#endif
