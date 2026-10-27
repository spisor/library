#include "segment.h"

#include <ctype.h>
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
