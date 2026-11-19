#ifndef RUNTIME_TEST_DECLARATION_FILE_H
#define RUNTIME_TEST_DECLARATION_FILE_H

#include <stdio.h>
#include <stdlib.h>

/** The whole of `runtime/library.json`, freed by the caller; NULL with a `length` of 0 when it cannot be read. */
static inline char *read_declaration_file(size_t *length) {
    FILE *file = fopen(RUNTIME_DECLARATION_PATH, "rb");
    char *text = NULL;
    long size = -1;
    *length = 0;
    if (file == NULL) {
        fprintf(stderr, "could not open %s\n", RUNTIME_DECLARATION_PATH);
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) == 0) {
        size = ftell(file);
    }
    if (size > 0 && fseek(file, 0, SEEK_SET) == 0) {
        text = malloc((size_t)size);
    }
    if (text != NULL && fread(text, 1, (size_t)size, file) == (size_t)size) {
        *length = (size_t)size;
    } else {
        free(text);
        text = NULL;
    }
    fclose(file);
    return text;
}

#endif
