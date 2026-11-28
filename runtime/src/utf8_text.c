#include "utf8_text.h"
#include "allocation.h"

#include <string.h>

#define REPLACEMENT "\xEF\xBF\xBD"
#define REPLACEMENT_LENGTH 3
#define LONGEST_SEQUENCE 4

static int is_continuation(unsigned char byte) {
    return (byte & 0xC0) == 0x80;
}

static size_t declared_length(unsigned char lead) {
    return lead >= 0x01 && lead < 0x80   ? 1
           : lead >= 0xC2 && lead <= 0xDF ? 2
           : lead >= 0xE0 && lead <= 0xEF ? 3
           : lead >= 0xF0 && lead <= 0xF4 ? 4
                                          : 0;
}

/** Whether the second byte keeps the sequence off overlong forms, surrogates and code points past U+10FFFF. */
static int is_allowed_second(unsigned char lead, unsigned char second) {
    return !((lead == 0xE0 && second < 0xA0) || (lead == 0xED && second > 0x9F) || (lead == 0xF0 && second < 0x90) ||
             (lead == 0xF4 && second > 0x8F));
}

/** The length of the well-formed sequence at bytes, of the left bytes there are, 0 when none starts there. */
static size_t sequence_length(const unsigned char *bytes, size_t left) {
    size_t length = declared_length(bytes[0]);
    if (length == 0 || length > left || (length > 1 && !is_allowed_second(bytes[0], bytes[1]))) {
        return 0;
    }
    for (size_t index = 1; index < length; index++) {
        if (!is_continuation(bytes[index])) {
            return 0;
        }
    }
    return length;
}

char *runtime_utf8_text(const char *bytes, size_t length, int cut) {
    const unsigned char *at = (const unsigned char *)bytes;
    size_t index = 0;
    while (cut && index < length && index < LONGEST_SEQUENCE - 1 && is_continuation(at[index])) {
        index++;
    }
    char *text = spisor_memory_allocate((length - index) * REPLACEMENT_LENGTH + 1);
    size_t written = 0;
    while (text != NULL && index < length) {
        size_t sequence = sequence_length(at + index, length - index);
        memcpy(text + written, sequence == 0 ? REPLACEMENT : (const char *)at + index,
               sequence == 0 ? REPLACEMENT_LENGTH : sequence);
        written += sequence == 0 ? REPLACEMENT_LENGTH : sequence;
        index += sequence == 0 ? 1 : sequence;
    }
    if (text != NULL) {
        text[written] = '\0';
    }
    return text;
}
