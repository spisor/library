#include "spisor/json_write.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "spisor/javascript.h"

#define INDENT_WIDTH 2
#define MAX_ARRAY_INDEX 4294967294ULL
#define MAX_ARRAY_INDEX_DIGITS 10

typedef struct text_buffer {
    char *data;
    size_t length;
    size_t capacity;
    int out_of_memory;
} text_buffer;

static void append_bytes(text_buffer *buffer, const char *bytes, size_t count) {
    if (buffer->out_of_memory) {
        return;
    }
    if (buffer->length + count + 1 > buffer->capacity) {
        size_t capacity = buffer->capacity == 0 ? 256 : buffer->capacity;
        while (buffer->length + count + 1 > capacity) {
            capacity *= 2;
        }
        char *grown = realloc(buffer->data, capacity);
        if (grown == NULL) {
            buffer->out_of_memory = 1;
            return;
        }
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->length, bytes, count);
    buffer->length += count;
    buffer->data[buffer->length] = '\0';
}

static void append_text(text_buffer *buffer, const char *text) {
    append_bytes(buffer, text, strlen(text));
}

static void append_indent(text_buffer *buffer, int depth) {
    for (int step = 0; step < depth * INDENT_WIDTH; step++) {
        append_bytes(buffer, " ", 1);
    }
}

static const char *short_escape(unsigned char byte) {
    switch (byte) {
    case '"': return "\\\"";
    case '\\': return "\\\\";
    case '\b': return "\\b";
    case '\f': return "\\f";
    case '\n': return "\\n";
    case '\r': return "\\r";
    case '\t': return "\\t";
    default: return NULL;
    }
}

static void append_control_escape(text_buffer *buffer, unsigned char byte) {
    static const char hex_digits[] = "0123456789abcdef";
    char escape[6] = {'\\', 'u', '0', '0', hex_digits[byte >> 4], hex_digits[byte & 0x0f]};
    append_bytes(buffer, escape, sizeof(escape));
}

static void append_quoted(text_buffer *buffer, const char *text) {
    append_bytes(buffer, "\"", 1);
    for (const char *cursor = text; *cursor != '\0'; cursor++) {
        unsigned char byte = (unsigned char)*cursor;
        const char *escape = short_escape(byte);
        if (escape != NULL) {
            append_text(buffer, escape);
        } else if (byte < 0x20) {
            append_control_escape(buffer, byte);
        } else {
            append_bytes(buffer, cursor, 1);
        }
    }
    append_bytes(buffer, "\"", 1);
}

static void append_number(text_buffer *buffer, const cJSON *number) {
    if (!isfinite(number->valuedouble)) {
        append_text(buffer, "null");
        return;
    }
    char *text = spisor_javascript_string(number);
    if (text == NULL) {
        buffer->out_of_memory = 1;
        return;
    }
    append_text(buffer, text);
    free(text);
}

/** The value of a key that is a canonical array index, or -1 when it is any other string. */
static int64_t array_index_of(const char *key) {
    size_t length = strlen(key);
    if (length == 0 || length > MAX_ARRAY_INDEX_DIGITS || (key[0] == '0' && length > 1)) {
        return -1;
    }
    uint64_t value = 0;
    for (size_t position = 0; position < length; position++) {
        if (key[position] < '0' || key[position] > '9') {
            return -1;
        }
        value = value * 10 + (uint64_t)(key[position] - '0');
    }
    return value <= MAX_ARRAY_INDEX ? (int64_t)value : -1;
}

typedef struct ordered_member {
    const cJSON *member;
    int64_t index;
} ordered_member;

static int compare_by_index(const void *left, const void *right) {
    int64_t left_index = ((const ordered_member *)left)->index;
    int64_t right_index = ((const ordered_member *)right)->index;
    return (left_index > right_index) - (left_index < right_index);
}

/** Members in the order JavaScript enumerates them, or NULL when memory runs out. */
static ordered_member *members_in_javascript_order(const cJSON *object, size_t count) {
    ordered_member *ordered = malloc(count * sizeof(ordered_member));
    if (ordered == NULL) {
        return NULL;
    }
    size_t indexed = 0;
    const cJSON *member = NULL;
    cJSON_ArrayForEach(member, object) {
        int64_t index = array_index_of(member->string);
        if (index >= 0) {
            ordered[indexed].member = member;
            ordered[indexed].index = index;
            indexed++;
        }
    }
    qsort(ordered, indexed, sizeof(ordered_member), compare_by_index);
    size_t placed = indexed;
    cJSON_ArrayForEach(member, object) {
        if (array_index_of(member->string) < 0) {
            ordered[placed].member = member;
            ordered[placed].index = -1;
            placed++;
        }
    }
    return ordered;
}

static void append_value(text_buffer *buffer, const cJSON *value, int depth);

static void append_array(text_buffer *buffer, const cJSON *array, int depth) {
    int count = cJSON_GetArraySize(array);
    if (count == 0) {
        append_text(buffer, "[]");
        return;
    }
    append_text(buffer, "[\n");
    const cJSON *element = NULL;
    int position = 0;
    cJSON_ArrayForEach(element, array) {
        append_indent(buffer, depth + 1);
        append_value(buffer, element, depth + 1);
        append_text(buffer, ++position < count ? ",\n" : "\n");
    }
    append_indent(buffer, depth);
    append_bytes(buffer, "]", 1);
}

static void append_object(text_buffer *buffer, const cJSON *object, int depth) {
    size_t count = (size_t)cJSON_GetArraySize(object);
    if (count == 0) {
        append_text(buffer, "{}");
        return;
    }
    ordered_member *ordered = members_in_javascript_order(object, count);
    if (ordered == NULL) {
        buffer->out_of_memory = 1;
        return;
    }
    append_text(buffer, "{\n");
    for (size_t position = 0; position < count; position++) {
        append_indent(buffer, depth + 1);
        append_quoted(buffer, ordered[position].member->string);
        append_text(buffer, ": ");
        append_value(buffer, ordered[position].member, depth + 1);
        append_text(buffer, position + 1 < count ? ",\n" : "\n");
    }
    append_indent(buffer, depth);
    append_bytes(buffer, "}", 1);
    free(ordered);
}

static void append_value(text_buffer *buffer, const cJSON *value, int depth) {
    if (cJSON_IsString(value)) {
        append_quoted(buffer, value->valuestring);
    } else if (cJSON_IsNumber(value)) {
        append_number(buffer, value);
    } else if (cJSON_IsTrue(value)) {
        append_text(buffer, "true");
    } else if (cJSON_IsFalse(value)) {
        append_text(buffer, "false");
    } else if (cJSON_IsArray(value)) {
        append_array(buffer, value, depth);
    } else if (cJSON_IsObject(value)) {
        append_object(buffer, value, depth);
    } else {
        append_text(buffer, "null");
    }
}

char *spisor_json_stringify_pretty(const cJSON *value) {
    text_buffer buffer = {NULL, 0, 0, 0};
    append_value(&buffer, value, 0);
    if (buffer.out_of_memory) {
        free(buffer.data);
        return NULL;
    }
    return buffer.data;
}
