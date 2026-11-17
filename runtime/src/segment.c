#include "segment.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "spisor/javascript.h"

static int is_reserved_device_stem(const char *stem, size_t length) {
    char upper[5];
    if (length != 3 && length != 4) {
        return 0;
    }
    for (size_t index = 0; index < length; index++) {
        upper[index] = (char)toupper((unsigned char)stem[index]);
    }
    upper[length] = '\0';
    if (length == 3) {
        return strcmp(upper, "CON") == 0 || strcmp(upper, "PRN") == 0 || strcmp(upper, "AUX") == 0 ||
               strcmp(upper, "NUL") == 0;
    }
    return (strncmp(upper, "COM", 3) == 0 || strncmp(upper, "LPT", 3) == 0) && upper[3] >= '1' && upper[3] <= '9';
}

static int has_reserved_device_stem(const char *segment) {
    return is_reserved_device_stem(segment, strcspn(segment, "."));
}

static int has_control_character(const char *segment) {
    for (const unsigned char *cursor = (const unsigned char *)segment; *cursor != '\0'; cursor++) {
        if (*cursor < 0x20) {
            return 1;
        }
    }
    return 0;
}

static int is_portable(const char *segment) {
    size_t length = strlen(segment);
    if (length == 0 || strcmp(segment, ".") == 0 || strcmp(segment, "..") == 0) {
        return 0;
    }
    if (strpbrk(segment, "/\\:<>\"|?*") != NULL || has_control_character(segment)) {
        return 0;
    }
    if (segment[length - 1] == '.' || segment[length - 1] == ' ') {
        return 0;
    }
    return !has_reserved_device_stem(segment);
}

char *runtime_portable_segment(const char *name) {
    char *trimmed = spisor_javascript_trim(name);
    if (trimmed != NULL && !is_portable(trimmed)) {
        free(trimmed);
        return NULL;
    }
    return trimmed;
}

/** The code point at text[*index], advancing past it; a byte that starts no whole sequence counts as U+FFFD alone. */
static uint32_t next_code_point(const char *text, size_t *index) {
    const unsigned char *bytes = (const unsigned char *)text + *index;
    size_t count = bytes[0] < 0x80 ? 1 : bytes[0] >= 0xF0 ? 4 : bytes[0] >= 0xE0 ? 3 : 2;
    for (size_t offset = 1; offset < count; offset++) {
        if ((bytes[offset] & 0xC0) != 0x80) {
            *index += 1;
            return 0xFFFD;
        }
    }
    return spisor_utf8_next(text, index);
}

/** Whether no JavaScript space starts or ends text, read without allocating. */
static int is_trimmed(const char *text) {
    size_t index = 0;
    uint32_t last = 0;
    while (text[index] != '\0') {
        size_t start = index;
        last = next_code_point(text, &index);
        if (start == 0 && spisor_is_javascript_space(last)) {
            return 0;
        }
    }
    return !spisor_is_javascript_space(last);
}

int runtime_is_portable_segment(const char *name) {
    return is_trimmed(name) && is_portable(name);
}
