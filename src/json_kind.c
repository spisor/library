#include "spisor/json_kind.h"

#include "allocation.h"
#include "json_parse.h"

#include "json_grammar.h"
#include "utf8.h"

#include <stdlib.h>
#include <string.h>

void spisor_release(uint8_t *buffer) {
    spisor_memory_release(buffer);
}

typedef enum document_refusal {
    DOCUMENT_ACCEPTED,
    DOCUMENT_NOT_UTF8,
    DOCUMENT_CARRIES_NUL,
    DOCUMENT_NOT_JSON,
    DOCUMENT_OUT_OF_MEMORY
} document_refusal;

#define NOT_AN_ESCAPED_UNIT (-1L)
#define ESCAPE_WIDTH 6

static int hex_value(char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

static long escaped_unit_at(const char *text, size_t length, size_t at) {
    if (at + ESCAPE_WIDTH > length || text[at] != '\\' || text[at + 1] != 'u') {
        return NOT_AN_ESCAPED_UNIT;
    }
    long unit = 0;
    for (size_t offset = 2; offset < ESCAPE_WIDTH; offset++) {
        int digit = hex_value(text[at + offset]);
        if (digit < 0) {
            return NOT_AN_ESCAPED_UNIT;
        }
        unit = unit * 16 + digit;
    }
    return unit;
}

static int is_high_surrogate(long unit) {
    return unit >= 0xD800 && unit <= 0xDBFF;
}

static int is_low_surrogate(long unit) {
    return unit >= 0xDC00 && unit <= 0xDFFF;
}

/** Answers how many bytes past the backslash the escape at `at` covers, rewriting it in place if unpaired. */
static size_t rewrite_escape(char *text, size_t length, size_t at, document_refusal *refusal) {
    long unit = escaped_unit_at(text, length, at);
    if (unit == NOT_AN_ESCAPED_UNIT) {
        return 1;
    }
    if (unit == 0) {
        *refusal = DOCUMENT_CARRIES_NUL;
        return 0;
    }
    if (is_high_surrogate(unit) && is_low_surrogate(escaped_unit_at(text, length, at + ESCAPE_WIDTH))) {
        return 2 * ESCAPE_WIDTH - 1;
    }
    if (is_high_surrogate(unit) || is_low_surrogate(unit)) {
        memcpy(text + at, "\\uFFFD", ESCAPE_WIDTH);
    }
    return ESCAPE_WIDTH - 1;
}

static document_refusal rewrite_escapes(char *text, size_t length) {
    document_refusal refusal = DOCUMENT_ACCEPTED;
    int in_string = 0;
    for (size_t index = 0; index < length && refusal == DOCUMENT_ACCEPTED; index++) {
        char character = text[index];
        if (character == '\0') {
            return DOCUMENT_NOT_JSON;
        }
        if (character == '"') {
            in_string = !in_string;
        } else if (in_string && character == '\\') {
            index += rewrite_escape(text, length, index, &refusal);
        }
    }
    return refusal;
}

static cJSON *detach_later_duplicates(cJSON *object, cJSON *member) {
    cJSON *last = NULL;
    cJSON *later = member->next;
    while (later != NULL) {
        cJSON *next = later->next;
        if (later->string != NULL && strcmp(later->string, member->string) == 0) {
            cJSON_Delete(last);
            last = cJSON_DetachItemViaPointer(object, later);
        }
        later = next;
    }
    return last;
}

static void keep_last_of_each_key(cJSON *item) {
    cJSON *child = item->child;
    while (child != NULL) {
        if (cJSON_IsObject(item) && child->string != NULL) {
            cJSON *last = detach_later_duplicates(item, child);
            if (last != NULL) {
                cJSON_ReplaceItemViaPointer(item, child, last);
                child = last;
            }
        }
        keep_last_of_each_key(child);
        child = child->next;
    }
}

/** Whether a text of the JSON grammar nests deeper than cJSON parses, which it then refuses as no JSON. */
static int nests_past_cjson(const char *text, size_t length) {
    size_t depth = 0;
    int in_string = 0;
    for (size_t index = 0; index < length; index++) {
        char character = text[index];
        if (in_string) {
            index += character == '\\';
            in_string = character != '"';
        } else if (character == '"') {
            in_string = 1;
        } else if (character == '[' || character == '{') {
            if (++depth > CJSON_NESTING_LIMIT) {
                return 1;
            }
        } else if (character == ']' || character == '}') {
            depth--;
        }
    }
    return 0;
}

static document_refusal parse_document(const uint8_t *bytes, size_t length, cJSON **root) {
    *root = NULL;
    if (!spisor_is_valid_utf8(bytes, length)) {
        return DOCUMENT_NOT_UTF8;
    }
    char *text = spisor_memory_allocate(length + 1);
    if (text == NULL) {
        return DOCUMENT_OUT_OF_MEMORY;
    }
    if (length > 0) {
        memcpy(text, bytes, length);
    }
    text[length] = '\0';
    document_refusal refusal = spisor_is_json_parse_grammar(text, length) ? rewrite_escapes(text, length)
                                                                          : DOCUMENT_NOT_JSON;
    if (refusal == DOCUMENT_ACCEPTED) {
        *root = cJSON_ParseWithLength(text, length);
        if (*root == NULL) {
            refusal = nests_past_cjson(text, length) ? DOCUMENT_NOT_JSON : DOCUMENT_OUT_OF_MEMORY;
        }
    }
    spisor_memory_release(text);
    if (*root != NULL) {
        keep_last_of_each_key(*root);
    }
    return refusal;
}

cJSON *spisor_parse_json(const uint8_t *bytes, size_t length) {
    cJSON *root = NULL;
    parse_document(bytes, length, &root);
    return root;
}

cJSON *spisor_parse_json_reporting(const uint8_t *bytes, size_t length, int *out_of_memory) {
    cJSON *root = NULL;
    *out_of_memory = parse_document(bytes, length, &root) == DOCUMENT_OUT_OF_MEMORY;
    return root;
}
