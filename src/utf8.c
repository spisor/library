#include "utf8.h"

static size_t sequence_length(uint8_t lead) {
    if (lead < 0x80) {
        return 1;
    }
    if (lead >= 0xC2 && lead <= 0xDF) {
        return 2;
    }
    if (lead >= 0xE0 && lead <= 0xEF) {
        return 3;
    }
    if (lead >= 0xF0 && lead <= 0xF4) {
        return 4;
    }
    return 0;
}

static int is_continuation(uint8_t byte) {
    return (byte & 0xC0) == 0x80;
}

/** The second byte's range is what rules out overlong forms, surrogates and code points past U+10FFFF. */
static int second_byte_in_range(uint8_t lead, uint8_t second) {
    switch (lead) {
    case 0xE0:
        return second >= 0xA0;
    case 0xED:
        return second <= 0x9F;
    case 0xF0:
        return second >= 0x90;
    case 0xF4:
        return second <= 0x8F;
    default:
        return 1;
    }
}

int spisor_is_valid_utf8(const uint8_t *bytes, size_t length) {
    size_t index = 0;
    while (index < length) {
        size_t width = sequence_length(bytes[index]);
        if (width == 0 || index + width > length) {
            return 0;
        }
        for (size_t offset = 1; offset < width; offset++) {
            if (!is_continuation(bytes[index + offset])) {
                return 0;
            }
        }
        if (width > 2 && !second_byte_in_range(bytes[index], bytes[index + 1])) {
            return 0;
        }
        index += width;
    }
    return 1;
}
