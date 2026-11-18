#ifndef RUNTIME_TEST_TIMESTAMP_TEXT_H
#define RUNTIME_TEST_TIMESTAMP_TEXT_H

#include "spisor/allocation.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TIMESTAMP_PLACEHOLDER "<time>"

static inline int timestamp_digits(const char *text, size_t count) {
    for (size_t index = 0; index < count; index++) {
        if (text[index] < '0' || text[index] > '9') {
            return 0;
        }
    }
    return 1;
}

/** The length of the ISO 8601 UTC timestamp starting text, or 0 when none starts it. */
static inline size_t timestamp_length(const char *text, size_t remaining) {
    if (remaining < 20 || !timestamp_digits(text, 4) || text[4] != '-' || !timestamp_digits(text + 5, 2) ||
        text[7] != '-' || !timestamp_digits(text + 8, 2) || text[10] != 'T' || !timestamp_digits(text + 11, 2) ||
        text[13] != ':' || !timestamp_digits(text + 14, 2) || text[16] != ':' || !timestamp_digits(text + 17, 2)) {
        return 0;
    }
    size_t length = 19;
    if (text[length] == '.') {
        length++;
        while (length < remaining && timestamp_digits(text + length, 1)) {
            length++;
        }
    }
    return length < remaining && text[length] == 'Z' ? length + 1 : 0;
}

/**
 * content with every timestamp replaced, so two runs at different times read alike, NUL-terminated and its length in
 * *normalized_length; NULL when memory runs out.
 */
static inline char *without_timestamps(const uint8_t *content, size_t length, size_t *normalized_length) {
    char *normalized = spisor_memory_allocate(length + 1);
    size_t written = 0;
    for (size_t index = 0; normalized != NULL && index < length;) {
        size_t matched = timestamp_length((const char *)content + index, length - index);
        if (matched > 0) {
            memcpy(normalized + written, TIMESTAMP_PLACEHOLDER, sizeof TIMESTAMP_PLACEHOLDER - 1);
            written += sizeof TIMESTAMP_PLACEHOLDER - 1;
            index += matched;
        } else {
            normalized[written++] = (char)content[index++];
        }
    }
    if (normalized != NULL) {
        normalized[written] = 0;
    }
    *normalized_length = written;
    return normalized;
}

#endif
