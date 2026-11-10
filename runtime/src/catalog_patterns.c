#include "catalog_patterns.h"

#include <stddef.h>
#include <string.h>

#include "identity.h"

static int is_ascii_digit(char character) {
    return character >= '0' && character <= '9';
}

static int is_ascii_letter(char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
}

static int is_reserved_device_name(const char *key) {
    static const char *const names[] = {"con", "prn", "aux", "nul"};
    for (size_t index = 0; index < sizeof names / sizeof names[0]; index++) {
        if (strcmp(key, names[index]) == 0) {
            return 1;
        }
    }
    int numbered = strncmp(key, "com", 3) == 0 || strncmp(key, "lpt", 3) == 0;
    return numbered && key[3] >= '1' && key[3] <= '9' && key[4] == 0;
}

static int is_item_name(const char *key) {
    size_t length = strlen(key);
    if (length == 0 || key[0] == '-' || key[length - 1] == '-' || strstr(key, "--") != NULL) {
        return 0;
    }
    for (const char *at = key; *at != 0; at++) {
        if (!((*at >= 'a' && *at <= 'z') || is_ascii_digit(*at) || *at == '-')) {
            return 0;
        }
    }
    return !is_reserved_device_name(key);
}

int runtime_catalog_is_item_id(const char *key) {
    runtime_identity identity;
    return is_item_name(key) || runtime_identity_parse_id(key, &identity);
}

static const char *after_digits(const char *at) {
    if (!is_ascii_digit(*at)) {
        return NULL;
    }
    while (is_ascii_digit(*at)) {
        at++;
    }
    return at;
}

static int is_identifier_character(char character) {
    return is_ascii_letter(character) || is_ascii_digit(character) || character == '.' || character == '-';
}

/** A run of [0-9A-Za-z.-] that does not end in a dot, stopping before stop or the end. */
static const char *after_identifiers(const char *at, char stop) {
    const char *start = at;
    while (*at != 0 && *at != stop) {
        if (!is_identifier_character(*at)) {
            return NULL;
        }
        at++;
    }
    return at == start || at[-1] == '.' ? NULL : at;
}

int runtime_catalog_is_version(const char *key) {
    if (strstr(key, "..") != NULL) {
        return 0;
    }
    const char *at = key;
    for (int part = 0; part < 3; part++) {
        at = after_digits(at);
        if (at == NULL || (part < 2 && *at++ != '.')) {
            return 0;
        }
    }
    if (*at == '-') {
        at = after_identifiers(at + 1, '+');
        if (at == NULL) {
            return 0;
        }
    }
    if (*at == '+') {
        at = after_identifiers(at + 1, 0);
        if (at == NULL) {
            return 0;
        }
    }
    return *at == 0;
}

static int is_file_key_segment(const char *start, const char *end) {
    if (end == start || end[-1] == '.' || end[-1] == ' ') {
        return 0;
    }
    for (const char *at = start; at < end; at++) {
        if (*at == '\\' || *at == ':') {
            return 0;
        }
    }
    return 1;
}

int runtime_catalog_is_file_key(const char *key) {
    const char *start = key;
    for (;;) {
        const char *end = strchr(start, '/');
        if (end == NULL) {
            return is_file_key_segment(start, start + strlen(start));
        }
        if (!is_file_key_segment(start, end)) {
            return 0;
        }
        start = end + 1;
    }
}

int runtime_catalog_is_sha256(const char *text) {
    size_t length = 0;
    for (const char *at = text; *at != 0; at++, length++) {
        if (!(is_ascii_digit(*at) || (*at >= 'a' && *at <= 'f'))) {
            return 0;
        }
    }
    return length == 64;
}

int runtime_catalog_is_name(const char *text) {
    if (!(is_ascii_letter(text[0]) || is_ascii_digit(text[0]))) {
        return 0;
    }
    size_t length = 1;
    for (const char *at = text + 1; *at != 0; at++, length++) {
        if (!(is_ascii_letter(*at) || is_ascii_digit(*at) || *at == '.' || *at == '_' || *at == '-')) {
            return 0;
        }
    }
    return length <= 64;
}

int runtime_catalog_is_candidate(const char *text) {
    if ((text[0] == '~' && text[1] == '/') || text[0] == '/') {
        return 1;
    }
    return is_ascii_letter(text[0]) && text[1] == ':' && text[2] == '/';
}
