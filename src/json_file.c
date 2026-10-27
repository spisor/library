#include "spisor/json_file.h"

#include <stdlib.h>
#include <string.h>

#include "spisor/file.h"
#include "spisor/javascript.h"
#include "spisor/json_kind.h"

#include "file_presence.h"

/** True when the code point ending just before index is a JavaScript LineTerminator, which is where a multiline ^ holds. */
static int follows_line_terminator(const unsigned char *text, size_t index) {
    if (index == 0) {
        return 1;
    }
    unsigned char last = text[index - 1];
    if (last == '\n' || last == '\r') {
        return 1;
    }
    return index >= 3 && text[index - 3] == 0xE2 && text[index - 2] == 0x80 && (last == 0xA8 || last == 0xA9);
}

/** Where a comment match starting at index ends, or index itself when no match starts there. */
static size_t comment_match_end(const char *text, size_t length, size_t index) {
    if (!follows_line_terminator((const unsigned char *)text, index)) {
        return index;
    }
    size_t cursor = index;
    while (cursor < length) {
        size_t next = cursor;
        if (!spisor_is_javascript_space(spisor_utf8_next(text, &next))) {
            break;
        }
        cursor = next;
    }
    if (cursor + 1 >= length || text[cursor] != '/' || text[cursor + 1] != '/') {
        return index;
    }
    const char *line_feed = memchr(text + cursor, '\n', length - cursor);
    return line_feed == NULL ? length : (size_t)(line_feed - text);
}

char *spisor_strip_comment_lines(const char *text, size_t length, size_t *stripped_length) {
    char *kept = malloc(length + 1);
    if (kept == NULL) {
        return NULL;
    }
    size_t kept_length = 0;
    size_t index = 0;
    while (index < length) {
        size_t end = comment_match_end(text, length, index);
        if (end > index) {
            index = end;
            continue;
        }
        size_t next = index;
        spisor_utf8_next(text, &next);
        memcpy(kept + kept_length, text + index, next - index);
        kept_length += next - index;
        index = next;
    }
    kept[kept_length] = '\0';
    *stripped_length = kept_length;
    return kept;
}

static cJSON *parse_stripped(const uint8_t *bytes, size_t length, spisor_json_file_state *state) {
    size_t decoded_length = 0;
    uint8_t *decoded = spisor_decode_utf8_lossy(bytes, length, &decoded_length);
    if (decoded == NULL) {
        return NULL;
    }
    size_t text_length = 0;
    char *text = spisor_strip_comment_lines((const char *)decoded, decoded_length, &text_length);
    free(decoded);
    if (text == NULL) {
        return NULL;
    }
    cJSON *parsed = spisor_parse_json((const uint8_t *)text, text_length);
    free(text);
    *state = parsed == NULL ? SPISOR_JSON_FILE_UNREADABLE : SPISOR_JSON_FILE_PARSED;
    return parsed;
}

cJSON *spisor_read_json_file(const char *path, spisor_json_file_state *state) {
    spisor_file_presence presence = spisor_probe_file(path);
    if (presence != SPISOR_FILE_PRESENT) {
        *state = presence == SPISOR_FILE_MISSING ? SPISOR_JSON_FILE_ABSENT : SPISOR_JSON_FILE_UNREADABLE;
        return NULL;
    }
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(path, &length);
    if (bytes == NULL) {
        *state = SPISOR_JSON_FILE_UNREADABLE;
        return NULL;
    }
    cJSON *parsed = parse_stripped(bytes, length, state);
    free(bytes);
    return parsed;
}
