#include "catalog_semver.h"

#include <stdlib.h>
#include <string.h>

typedef struct parsed_version {
    double core[3];
    const char *prerelease;
    size_t prerelease_length;
} parsed_version;

static int is_digit(char character) {
    return character >= '0' && character <= '9';
}

static int is_identifier_character(char character) {
    return is_digit(character) || (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           character == '.' || character == '-';
}

static const char *after_number(const char *at, double *value) {
    if (!is_digit(*at)) {
        return NULL;
    }
    *value = 0;
    while (is_digit(*at)) {
        *value = *value * 10 + (*at++ - '0');
    }
    return at;
}

static const char *after_identifiers(const char *at) {
    const char *start = at;
    while (is_identifier_character(*at)) {
        at++;
    }
    return at == start ? NULL : at;
}

static int parse_version(const char *text, parsed_version *version) {
    const char *at = text;
    for (int part = 0; part < 3; part++) {
        at = after_number(at, &version->core[part]);
        if (at == NULL || (part < 2 && *at++ != '.')) {
            return 0;
        }
    }
    version->prerelease = NULL;
    version->prerelease_length = 0;
    if (*at == '-') {
        const char *end = after_identifiers(at + 1);
        if (end == NULL) {
            return 0;
        }
        version->prerelease = at + 1;
        version->prerelease_length = (size_t)(end - at - 1);
        at = end;
    }
    if (*at == '+') {
        at = after_identifiers(at + 1);
        if (at == NULL) {
            return 0;
        }
    }
    return *at == 0;
}

static int is_numeric(const char *start, size_t length) {
    if (length == 0) {
        return 0;
    }
    for (size_t index = 0; index < length; index++) {
        if (!is_digit(start[index])) {
            return 0;
        }
    }
    return 1;
}

static double number_of(const char *start, size_t length) {
    double value = 0;
    for (size_t index = 0; index < length; index++) {
        value = value * 10 + (start[index] - '0');
    }
    return value;
}

static int sign_of(double difference) {
    return difference < 0 ? -1 : difference > 0 ? 1 : 0;
}

static int compare_text(const char *left, size_t left_length, const char *right, size_t right_length) {
    size_t shorter = left_length < right_length ? left_length : right_length;
    int order = memcmp(left, right, shorter);
    if (order != 0) {
        return order < 0 ? -1 : 1;
    }
    return sign_of((double)left_length - (double)right_length);
}

static int compare_identifiers(const char *left, size_t left_length, const char *right, size_t right_length) {
    int left_numeric = is_numeric(left, left_length);
    int right_numeric = is_numeric(right, right_length);
    if (left_numeric && right_numeric) {
        return sign_of(number_of(left, left_length) - number_of(right, right_length));
    }
    if (left_numeric != right_numeric) {
        return left_numeric ? -1 : 1;
    }
    return compare_text(left, left_length, right, right_length);
}

static size_t identifier_count(const parsed_version *version) {
    if (version->prerelease == NULL) {
        return 0;
    }
    size_t count = 1;
    for (size_t index = 0; index < version->prerelease_length; index++) {
        count += version->prerelease[index] == '.';
    }
    return count;
}

static size_t identifier_length(const char *start, const char *end) {
    const char *dot = memchr(start, '.', (size_t)(end - start));
    return (size_t)((dot == NULL ? end : dot) - start);
}

static int compare_prereleases(const parsed_version *left, const parsed_version *right) {
    size_t left_count = identifier_count(left);
    size_t right_count = identifier_count(right);
    if (left_count == 0 || right_count == 0) {
        return sign_of((double)right_count - (double)left_count);
    }
    const char *left_at = left->prerelease;
    const char *right_at = right->prerelease;
    const char *left_end = left->prerelease + left->prerelease_length;
    const char *right_end = right->prerelease + right->prerelease_length;
    for (size_t index = 0; index < left_count && index < right_count; index++) {
        size_t left_length = identifier_length(left_at, left_end);
        size_t right_length = identifier_length(right_at, right_end);
        int order = compare_identifiers(left_at, left_length, right_at, right_length);
        if (order != 0) {
            return order;
        }
        left_at += left_length + 1;
        right_at += right_length + 1;
    }
    return sign_of((double)left_count - (double)right_count);
}

int runtime_catalog_compare_versions(const char *left, const char *right) {
    parsed_version parsed_left;
    parsed_version parsed_right;
    int left_parses = parse_version(left, &parsed_left);
    int right_parses = parse_version(right, &parsed_right);
    if (left_parses && right_parses) {
        for (int part = 0; part < 3; part++) {
            int order = sign_of(parsed_left.core[part] - parsed_right.core[part]);
            if (order != 0) {
                return order;
            }
        }
        return compare_prereleases(&parsed_left, &parsed_right);
    }
    if (left_parses != right_parses) {
        return left_parses ? 1 : -1;
    }
    return compare_text(left, strlen(left), right, strlen(right));
}
