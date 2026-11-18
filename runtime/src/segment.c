#include "segment.h"
#include "allocation.h"

#include <ctype.h>
#include <stdint.h>
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

static int has_forbidden_character(const char *segment, size_t length) {
    for (size_t index = 0; index < length; index++) {
        unsigned char character = (unsigned char)segment[index];
        if (character < 0x20 || strchr("/\\:<>\"|?*", character) != NULL) {
            return 1;
        }
    }
    return 0;
}

static size_t stem_length(const char *segment, size_t length) {
    const char *dot = memchr(segment, '.', length);
    return dot == NULL ? length : (size_t)(dot - segment);
}

static int is_dot_name(const char *segment, size_t length) {
    return (length == 1 && segment[0] == '.') || (length == 2 && segment[0] == '.' && segment[1] == '.');
}

static int is_portable(const char *segment, size_t length) {
    if (length == 0 || is_dot_name(segment, length) || has_forbidden_character(segment, length)) {
        return 0;
    }
    if (segment[length - 1] == '.' || segment[length - 1] == ' ') {
        return 0;
    }
    return !is_reserved_device_stem(segment, stem_length(segment, length));
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

typedef struct text_range {
    size_t start;
    size_t end;
} text_range;

/** The part of text String.prototype.trim keeps, found without allocating. */
static text_range trimmed_range_of(const char *text) {
    text_range range = {0, 0};
    int found = 0;
    size_t index = 0;
    while (text[index] != '\0') {
        size_t start = index;
        if (!spisor_is_javascript_space(next_code_point(text, &index))) {
            range.start = found ? range.start : start;
            range.end = index;
            found = 1;
        }
    }
    return range;
}

spisor_status runtime_portable_segment(const char *name, char **segment) {
    text_range range = trimmed_range_of(name);
    size_t length = range.end - range.start;
    *segment = NULL;
    if (!is_portable(name + range.start, length)) {
        return SPISOR_MALFORMED;
    }
    char *trimmed = spisor_memory_allocate(length + 1);
    if (trimmed == NULL) {
        return SPISOR_OUT_OF_MEMORY;
    }
    memcpy(trimmed, name + range.start, length);
    trimmed[length] = '\0';
    *segment = trimmed;
    return SPISOR_OK;
}

int runtime_trims_to_portable_segment(const char *name) {
    text_range range = trimmed_range_of(name);
    return is_portable(name + range.start, range.end - range.start);
}

int runtime_is_portable_segment(const char *name) {
    size_t length = strlen(name);
    text_range range = trimmed_range_of(name);
    return range.start == 0 && range.end == length && is_portable(name, length);
}
