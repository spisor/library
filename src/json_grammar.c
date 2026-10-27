#include "json_grammar.h"

#include <string.h>

/** Deeper than cJSON's own nesting limit, so that limit, not this check, is the one that refuses. */
#define DEEPEST_NESTING 1100

typedef struct cursor {
    const unsigned char *text;
    size_t length;
    size_t at;
    int depth;
} cursor;

static int read_value(cursor *reader);

static int is_digit(unsigned char character) {
    return character >= '0' && character <= '9';
}

static int is_hex_digit(unsigned char character) {
    return is_digit(character) || (character >= 'a' && character <= 'f') || (character >= 'A' && character <= 'F');
}

static int at_end(const cursor *reader) {
    return reader->at >= reader->length;
}

static unsigned char peek(const cursor *reader) {
    return at_end(reader) ? 0 : reader->text[reader->at];
}

static int accept(cursor *reader, unsigned char character) {
    if (!at_end(reader) && reader->text[reader->at] == character) {
        reader->at++;
        return 1;
    }
    return 0;
}

static void skip_whitespace(cursor *reader) {
    while (!at_end(reader)) {
        unsigned char character = reader->text[reader->at];
        if (character != ' ' && character != '\t' && character != '\n' && character != '\r') {
            return;
        }
        reader->at++;
    }
}

static int read_digits(cursor *reader) {
    size_t start = reader->at;
    while (is_digit(peek(reader))) {
        reader->at++;
    }
    return reader->at > start;
}

static int read_number(cursor *reader) {
    accept(reader, '-');
    if (!accept(reader, '0') && !(is_digit(peek(reader)) && read_digits(reader))) {
        return 0;
    }
    if (accept(reader, '.') && !read_digits(reader)) {
        return 0;
    }
    if (accept(reader, 'e') || accept(reader, 'E')) {
        if (!accept(reader, '+')) {
            accept(reader, '-');
        }
        return read_digits(reader);
    }
    return 1;
}

static int read_escape(cursor *reader) {
    unsigned char escaped = peek(reader);
    reader->at++;
    if (strchr("\"\\/bfnrt", escaped) != NULL && escaped != 0) {
        return 1;
    }
    if (escaped != 'u') {
        return 0;
    }
    for (int digit = 0; digit < 4; digit++) {
        if (!is_hex_digit(peek(reader))) {
            return 0;
        }
        reader->at++;
    }
    return 1;
}

static int read_string(cursor *reader) {
    if (!accept(reader, '"')) {
        return 0;
    }
    while (!at_end(reader)) {
        unsigned char character = reader->text[reader->at++];
        if (character == '"') {
            return 1;
        }
        if (character < 0x20) {
            return 0;
        }
        if (character == '\\' && !read_escape(reader)) {
            return 0;
        }
    }
    return 0;
}

static int read_literal(cursor *reader, const char *literal) {
    size_t length = strlen(literal);
    if (reader->length - reader->at < length || memcmp(reader->text + reader->at, literal, length) != 0) {
        return 0;
    }
    reader->at += length;
    return 1;
}

static int read_members(cursor *reader, unsigned char close, int keyed) {
    skip_whitespace(reader);
    if (accept(reader, close)) {
        return 1;
    }
    do {
        skip_whitespace(reader);
        if (keyed) {
            if (!read_string(reader)) {
                return 0;
            }
            skip_whitespace(reader);
            if (!accept(reader, ':')) {
                return 0;
            }
        }
        if (!read_value(reader)) {
            return 0;
        }
        skip_whitespace(reader);
    } while (accept(reader, ','));
    return accept(reader, close);
}

static int read_container(cursor *reader, unsigned char close, int keyed) {
    if (++reader->depth > DEEPEST_NESTING) {
        return 0;
    }
    reader->at++;
    int read = read_members(reader, close, keyed);
    reader->depth--;
    return read;
}

static int read_value(cursor *reader) {
    skip_whitespace(reader);
    switch (peek(reader)) {
    case '{':
        return read_container(reader, '}', 1);
    case '[':
        return read_container(reader, ']', 0);
    case '"':
        return read_string(reader);
    case 't':
        return read_literal(reader, "true");
    case 'f':
        return read_literal(reader, "false");
    case 'n':
        return read_literal(reader, "null");
    default:
        return read_number(reader);
    }
}

int spisor_is_json_parse_grammar(const char *text, size_t length) {
    cursor reader = {(const unsigned char *)text, length, 0, 0};
    if (!read_value(&reader)) {
        return 0;
    }
    skip_whitespace(&reader);
    return at_end(&reader);
}
