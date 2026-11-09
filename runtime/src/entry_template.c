#include "entry_template.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#define OPEN "{{"
#define CLOSE "}}"
#define JSON_FORM "json:"

typedef struct placeholder {
    const char *name;
    size_t offset;
} placeholder;

static const placeholder PLACEHOLDERS[] = {
    {"home", offsetof(runtime_entry_values, home)},
    {"entryRoot", offsetof(runtime_entry_values, entry_root)},
    {"loaderId", offsetof(runtime_entry_values, loader_id)},
    {"boot", offsetof(runtime_entry_values, boot)},
    {"bootedHost", offsetof(runtime_entry_values, booted_host)},
    {"hint", offsetof(runtime_entry_values, hint)},
    {"bundles", offsetof(runtime_entry_values, bundles)},
};

/** One {{...}} found in a template: what it names, whether in its JSON form, and where it ends. */
typedef struct found_placeholder {
    const placeholder *known;
    int json;
    const char *end;
} found_placeholder;

typedef struct text_buffer {
    char *data;
    size_t length;
    size_t capacity;
    int failed;
} text_buffer;

static const placeholder *named(const char *name, size_t length) {
    for (size_t index = 0; index < sizeof PLACEHOLDERS / sizeof PLACEHOLDERS[0]; index++) {
        if (strlen(PLACEHOLDERS[index].name) == length && strncmp(PLACEHOLDERS[index].name, name, length) == 0) {
            return &PLACEHOLDERS[index];
        }
    }
    return NULL;
}

/** Reads the placeholder opening at start; end is NULL when it never closes. */
static found_placeholder read_placeholder(const char *start) {
    found_placeholder found = {NULL, 0, NULL};
    const char *name = start + strlen(OPEN);
    const char *close = strstr(name, CLOSE);
    if (close == NULL) {
        return found;
    }
    found.end = close + strlen(CLOSE);
    if (strncmp(name, JSON_FORM, strlen(JSON_FORM)) == 0) {
        found.json = 1;
        name += strlen(JSON_FORM);
    }
    found.known = named(name, (size_t)(close - name));
    return found;
}

static void append(text_buffer *buffer, const char *text, size_t length) {
    if (buffer->failed) {
        return;
    }
    if (buffer->length + length + 1 > buffer->capacity) {
        size_t capacity = (buffer->length + length + 1) * 2;
        char *grown = realloc(buffer->data, capacity);
        if (grown == NULL) {
            buffer->failed = 1;
            return;
        }
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
}

static void append_value(text_buffer *buffer, const char *value, int json) {
    if (!json) {
        append(buffer, value, strlen(value));
        return;
    }
    cJSON *string = cJSON_CreateString(value);
    char *printed = string == NULL ? NULL : cJSON_PrintUnformatted(string);
    if (printed == NULL) {
        buffer->failed = 1;
    } else {
        append(buffer, printed, strlen(printed));
    }
    cJSON_free(printed);
    cJSON_Delete(string);
}

char *runtime_entry_expand(const char *template_text, const runtime_entry_values *values) {
    text_buffer buffer = {NULL, 0, 0, 0};
    append(&buffer, "", 0);
    const char *rest = template_text;
    for (const char *open = strstr(rest, OPEN); open != NULL; open = strstr(rest, OPEN)) {
        found_placeholder found = read_placeholder(open);
        const char *after = found.end == NULL ? open + strlen(OPEN) : found.end;
        if (found.known == NULL) {
            append(&buffer, rest, (size_t)(after - rest));
        } else {
            append(&buffer, rest, (size_t)(open - rest));
            append_value(&buffer, *(const char *const *)(const void *)((const char *)values + found.known->offset), found.json);
        }
        rest = after;
    }
    append(&buffer, rest, strlen(rest));
    if (buffer.failed) {
        free(buffer.data);
        return NULL;
    }
    return buffer.data;
}

int runtime_entry_template_is_known(const char *template_text) {
    const char *open = strstr(template_text, OPEN);
    while (open != NULL) {
        found_placeholder found = read_placeholder(open);
        if (found.known == NULL) {
            return 0;
        }
        open = strstr(found.end, OPEN);
    }
    return 1;
}
