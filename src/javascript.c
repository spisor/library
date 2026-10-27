#include "spisor/javascript.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REPLACEMENT_CHARACTER 0xFFFDu
#define SHORTEST_ROUND_TRIP_DIGITS 17
#define LARGEST_PLAIN_EXPONENT 21
#define SMALLEST_PLAIN_EXPONENT (-6)

typedef struct sequence_rule {
    size_t continuations;
    uint32_t initial;
    uint8_t lower;
    uint8_t upper;
} sequence_rule;

/** The WHATWG UTF-8 decoder's table: how many bytes follow a lead, and the range the next one must fall in. */
static int sequence_rule_for(uint8_t lead, sequence_rule *rule) {
    rule->lower = 0x80;
    rule->upper = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
        rule->continuations = 1;
        rule->initial = lead & 0x1Fu;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        rule->continuations = 2;
        rule->initial = lead & 0x0Fu;
        rule->lower = lead == 0xE0 ? 0xA0 : 0x80;
        rule->upper = lead == 0xED ? 0x9F : 0xBF;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        rule->continuations = 3;
        rule->initial = lead & 0x07u;
        rule->lower = lead == 0xF0 ? 0x90 : 0x80;
        rule->upper = lead == 0xF4 ? 0x8F : 0xBF;
    } else {
        return 0;
    }
    return 1;
}

/** One code point decoded from bytes[*index], U+FFFD for a maximal invalid subpart, which is left unconsumed past its end. */
static uint32_t decode_one(const uint8_t *bytes, size_t length, size_t *index) {
    uint8_t lead = bytes[(*index)++];
    sequence_rule rule;
    if (lead < 0x80) {
        return lead;
    }
    if (!sequence_rule_for(lead, &rule)) {
        return REPLACEMENT_CHARACTER;
    }
    uint32_t value = rule.initial;
    size_t seen = 0;
    while (seen < rule.continuations && *index < length && bytes[*index] >= rule.lower && bytes[*index] <= rule.upper) {
        value = (value << 6) | (bytes[(*index)++] & 0x3Fu);
        rule.lower = 0x80;
        rule.upper = 0xBF;
        seen++;
    }
    return seen == rule.continuations ? value : REPLACEMENT_CHARACTER;
}

static size_t encode(uint32_t code_point, uint8_t *out) {
    if (code_point < 0x80) {
        out[0] = (uint8_t)code_point;
        return 1;
    }
    if (code_point < 0x800) {
        out[0] = (uint8_t)(0xC0 | (code_point >> 6));
        out[1] = (uint8_t)(0x80 | (code_point & 0x3F));
        return 2;
    }
    if (code_point < 0x10000) {
        out[0] = (uint8_t)(0xE0 | (code_point >> 12));
        out[1] = (uint8_t)(0x80 | ((code_point >> 6) & 0x3F));
        out[2] = (uint8_t)(0x80 | (code_point & 0x3F));
        return 3;
    }
    out[0] = (uint8_t)(0xF0 | (code_point >> 18));
    out[1] = (uint8_t)(0x80 | ((code_point >> 12) & 0x3F));
    out[2] = (uint8_t)(0x80 | ((code_point >> 6) & 0x3F));
    out[3] = (uint8_t)(0x80 | (code_point & 0x3F));
    return 4;
}

uint8_t *spisor_decode_utf8_lossy(const uint8_t *bytes, size_t length, size_t *out_length) {
    uint8_t *decoded = malloc(length * 3 + 1);
    if (decoded == NULL) {
        return NULL;
    }
    size_t written = 0;
    size_t index = 0;
    while (index < length) {
        written += encode(decode_one(bytes, length, &index), decoded + written);
    }
    decoded[written] = '\0';
    *out_length = written;
    return decoded;
}

uint32_t spisor_utf8_next(const char *text, size_t *index) {
    const unsigned char *bytes = (const unsigned char *)text;
    unsigned char lead = bytes[*index];
    size_t count = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
    uint32_t value = count == 1 ? lead : (uint32_t)(lead & (0x7F >> count));
    for (size_t offset = 1; offset < count; offset++) {
        value = (value << 6) | (bytes[*index + offset] & 0x3F);
    }
    *index += count;
    return value;
}

int spisor_is_javascript_space(uint32_t code_point) {
    switch (code_point) {
    case 0x09:
    case 0x0A:
    case 0x0B:
    case 0x0C:
    case 0x0D:
    case 0x20:
    case 0xA0:
    case 0x1680:
    case 0x2028:
    case 0x2029:
    case 0x202F:
    case 0x205F:
    case 0x3000:
    case 0xFEFF:
        return 1;
    default:
        return code_point >= 0x2000 && code_point <= 0x200A;
    }
}

static size_t last_code_point_start(const char *text, size_t end) {
    size_t start = end - 1;
    while (start > 0 && ((unsigned char)text[start] & 0xC0) == 0x80) {
        start--;
    }
    return start;
}

char *spisor_javascript_trim(const char *text) {
    size_t start = 0;
    size_t end = strlen(text);
    while (start < end) {
        size_t next = start;
        if (!spisor_is_javascript_space(spisor_utf8_next(text, &next))) {
            break;
        }
        start = next;
    }
    while (end > start) {
        size_t last = last_code_point_start(text, end);
        size_t probe = last;
        if (!spisor_is_javascript_space(spisor_utf8_next(text, &probe))) {
            break;
        }
        end = last;
    }
    char *trimmed = malloc(end - start + 1);
    if (trimmed != NULL) {
        memcpy(trimmed, text + start, end - start);
        trimmed[end - start] = '\0';
    }
    return trimmed;
}

static char *copy_of(const char *text) {
    size_t length = strlen(text);
    char *copy = malloc(length + 1);
    if (copy != NULL) {
        memcpy(copy, text, length + 1);
    }
    return copy;
}

typedef struct shortest_digits {
    char digits[SHORTEST_ROUND_TRIP_DIGITS + 1];
    int point;
} shortest_digits;

/**
 * The fewest significant digits that read back as the same double, and where the decimal point
 * falls among them, which is what Number.prototype.toString prints from.
 */
static shortest_digits shortest_digits_of(double magnitude) {
    char printed[64];
    for (int precision = 1; precision <= SHORTEST_ROUND_TRIP_DIGITS; precision++) {
        snprintf(printed, sizeof(printed), "%.*e", precision - 1, magnitude);
        if (strtod(printed, NULL) == magnitude) {
            break;
        }
    }
    shortest_digits shortest;
    size_t count = 0;
    const char *cursor = printed;
    for (; *cursor != 'e'; cursor++) {
        if (*cursor >= '0' && *cursor <= '9') {
            shortest.digits[count++] = *cursor;
        }
    }
    while (count > 1 && shortest.digits[count - 1] == '0') {
        count--;
    }
    shortest.digits[count] = '\0';
    shortest.point = atoi(cursor + 1) + 1;
    return shortest;
}

static void append_zeros(char *out, int count) {
    size_t length = strlen(out);
    for (int index = 0; index < count; index++) {
        out[length++] = '0';
    }
    out[length] = '\0';
}

static char *javascript_number(double value) {
    if (value == 0) {
        return copy_of("0");
    }
    if (isinf(value)) {
        return copy_of(value > 0 ? "Infinity" : "-Infinity");
    }
    shortest_digits shortest = shortest_digits_of(fabs(value));
    int count = (int)strlen(shortest.digits);
    int point = shortest.point;
    char out[64] = "";
    if (value < 0) {
        strcat(out, "-");
    }
    if (count <= point && point <= LARGEST_PLAIN_EXPONENT) {
        strcat(out, shortest.digits);
        append_zeros(out, point - count);
    } else if (0 < point && point <= LARGEST_PLAIN_EXPONENT) {
        strncat(out, shortest.digits, (size_t)point);
        strcat(out, ".");
        strcat(out, shortest.digits + point);
    } else if (SMALLEST_PLAIN_EXPONENT < point && point <= 0) {
        strcat(out, "0.");
        append_zeros(out, -point);
        strcat(out, shortest.digits);
    } else {
        strncat(out, shortest.digits, 1);
        if (count > 1) {
            strcat(out, ".");
            strcat(out, shortest.digits + 1);
        }
        char exponent[16];
        snprintf(exponent, sizeof(exponent), "e%c%d", point - 1 >= 0 ? '+' : '-', abs(point - 1));
        strcat(out, exponent);
    }
    return copy_of(out);
}

static char *concatenated(char *first, const char *separator, char *second) {
    char *joined = NULL;
    if (first != NULL && second != NULL) {
        size_t first_length = strlen(first);
        size_t separator_length = strlen(separator);
        size_t second_length = strlen(second);
        joined = malloc(first_length + separator_length + second_length + 1);
        if (joined != NULL) {
            memcpy(joined, first, first_length);
            memcpy(joined + first_length, separator, separator_length);
            memcpy(joined + first_length + separator_length, second, second_length + 1);
        }
    }
    free(first);
    free(second);
    return joined;
}

static char *javascript_array_join(const cJSON *array) {
    char *joined = copy_of("");
    const cJSON *element = NULL;
    int first = 1;
    cJSON_ArrayForEach(element, array) {
        char *part = cJSON_IsNull(element) ? copy_of("") : spisor_javascript_string(element);
        joined = concatenated(joined, first ? "" : ",", part);
        first = 0;
    }
    return joined;
}

char *spisor_javascript_string(const cJSON *value) {
    if (cJSON_IsString(value)) {
        return copy_of(value->valuestring);
    }
    if (cJSON_IsNumber(value)) {
        return javascript_number(value->valuedouble);
    }
    if (cJSON_IsTrue(value)) {
        return copy_of("true");
    }
    if (cJSON_IsFalse(value)) {
        return copy_of("false");
    }
    if (cJSON_IsArray(value)) {
        return javascript_array_join(value);
    }
    if (cJSON_IsObject(value)) {
        return copy_of("[object Object]");
    }
    return copy_of("null");
}
