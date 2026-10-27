#include "spisor/json_kind.h"

#include "json_grammar.h"

#include <stdlib.h>
#include <string.h>

typedef struct fact_rule {
    const char *name;
    int (*is_well_formed)(const cJSON *value);
    const char *fix;
} fact_rule;

static int is_string_map(const cJSON *value) {
    const cJSON *entry = NULL;
    if (!cJSON_IsObject(value)) {
        return 0;
    }
    cJSON_ArrayForEach(entry, value) {
        if (!cJSON_IsString(entry)) {
            return 0;
        }
    }
    return 1;
}

static int is_string(const cJSON *value) {
    return cJSON_IsString(value);
}

static int is_string_list(const cJSON *value) {
    const cJSON *entry = NULL;
    if (!cJSON_IsArray(value)) {
        return 0;
    }
    cJSON_ArrayForEach(entry, value) {
        if (!cJSON_IsString(entry)) {
            return 0;
        }
    }
    return 1;
}

static const fact_rule fact_rules[] = {
    {"environment", is_string_map, "send environment as an object of strings"},
    {"home", is_string, "send home as a string"},
    {"arguments", is_string_list, "send arguments as an array of strings"},
};

static int32_t out_of_memory(uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    return SPISOR_STATUS_OUT_OF_MEMORY;
}

static int32_t emit_printed(char *printed, int32_t status, uint8_t **out, size_t *out_len) {
    if (printed == NULL) {
        return out_of_memory(out, out_len);
    }
    *out = (uint8_t *)printed;
    *out_len = strlen(printed);
    return status;
}

int32_t spisor_succeed(const cJSON *result, uint8_t **out, size_t *out_len) {
    if (result == NULL) {
        return spisor_fail("the library produced no result", "report this as a defect in the library", out,
                           out_len);
    }
    return emit_printed(cJSON_PrintUnformatted(result), SPISOR_STATUS_SUCCESS, out, out_len);
}

int32_t spisor_fail(const char *message, const char *fix, uint8_t **out, size_t *out_len) {
    cJSON *failure = cJSON_CreateObject();
    if (failure == NULL || cJSON_AddStringToObject(failure, "message", message) == NULL ||
        cJSON_AddStringToObject(failure, "fix", fix) == NULL) {
        cJSON_Delete(failure);
        return out_of_memory(out, out_len);
    }
    char *printed = cJSON_PrintUnformatted(failure);
    cJSON_Delete(failure);
    return emit_printed(printed, SPISOR_STATUS_FAILURE, out, out_len);
}

void spisor_release(uint8_t *buffer) {
    free(buffer);
}

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

static int is_valid_utf8(const uint8_t *bytes, size_t length) {
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

static document_refusal parse_document(const uint8_t *bytes, size_t length, cJSON **root) {
    *root = NULL;
    if (!is_valid_utf8(bytes, length)) {
        return DOCUMENT_NOT_UTF8;
    }
    char *text = malloc(length + 1);
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
        refusal = *root == NULL ? DOCUMENT_NOT_JSON : DOCUMENT_ACCEPTED;
    }
    free(text);
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

static int32_t refuse_document(document_refusal refusal, uint8_t **out, size_t *out_len) {
    switch (refusal) {
    case DOCUMENT_NOT_UTF8:
        return spisor_fail("the input is not valid UTF-8", "send the argument as UTF-8 JSON", out, out_len);
    case DOCUMENT_CARRIES_NUL:
        return spisor_fail("the input carries a NUL character in a string",
                           "remove the NUL character; no path or name can hold one", out, out_len);
    case DOCUMENT_OUT_OF_MEMORY:
        return out_of_memory(out, out_len);
    default:
        return spisor_fail("the input is not valid JSON", "send exactly one JSON value", out, out_len);
    }
}

static const fact_rule *find_fact_rule(const char *name) {
    for (size_t index = 0; index < sizeof(fact_rules) / sizeof(fact_rules[0]); index++) {
        if (strcmp(fact_rules[index].name, name) == 0) {
            return &fact_rules[index];
        }
    }
    return NULL;
}

static int32_t fail_naming(const char *prefix, const char *name, const char *suffix, const char *fix,
                           uint8_t **out, size_t *out_len) {
    size_t prefix_length = strlen(prefix);
    size_t name_length = strlen(name);
    size_t suffix_length = strlen(suffix);
    char *message = malloc(prefix_length + name_length + suffix_length + 1);
    if (message == NULL) {
        return out_of_memory(out, out_len);
    }
    memcpy(message, prefix, prefix_length);
    memcpy(message + prefix_length, name, name_length);
    memcpy(message + prefix_length + name_length, suffix, suffix_length + 1);
    int32_t status = spisor_fail(message, fix, out, out_len);
    free(message);
    return status;
}

static int32_t check_facts(const cJSON *context, uint8_t **out, size_t *out_len) {
    const cJSON *fact = NULL;
    cJSON_ArrayForEach(fact, context) {
        const fact_rule *rule = find_fact_rule(fact->string);
        if (rule == NULL) {
            return fail_naming("the context carries an unknown fact ", fact->string, "",
                               "send only environment, home and arguments", out, out_len);
        }
        if (!rule->is_well_formed(fact)) {
            return fail_naming("the context fact ", rule->name, " is malformed", rule->fix, out, out_len);
        }
    }
    return SPISOR_STATUS_SUCCESS;
}

static int32_t split_envelope(cJSON *envelope, spisor_call *call, uint8_t **out, size_t *out_len) {
    static const char envelope_fix[] = "send {\"argument\": ..., \"context\": {...}}";
    if (!cJSON_IsObject(envelope)) {
        return spisor_fail("the envelope is not an object", envelope_fix, out, out_len);
    }
    cJSON *argument = cJSON_GetObjectItemCaseSensitive(envelope, "argument");
    cJSON *context = cJSON_GetObjectItemCaseSensitive(envelope, "context");
    if (argument == NULL) {
        return spisor_fail("the envelope has no argument", envelope_fix, out, out_len);
    }
    if (!cJSON_IsObject(context)) {
        return spisor_fail("the envelope has no context object", envelope_fix, out, out_len);
    }
    int32_t status = check_facts(context, out, out_len);
    if (status != SPISOR_STATUS_SUCCESS) {
        return status;
    }
    call->argument = cJSON_DetachItemViaPointer(envelope, argument);
    call->context = cJSON_DetachItemViaPointer(envelope, context);
    return SPISOR_STATUS_SUCCESS;
}

int32_t spisor_parse_call(const uint8_t *in, size_t in_len, int contextual, spisor_call *call,
                          uint8_t **out, size_t *out_len) {
    call->argument = NULL;
    call->context = NULL;
    *out = NULL;
    *out_len = 0;
    cJSON *root = NULL;
    document_refusal refusal = parse_document(in, in_len, &root);
    if (refusal != DOCUMENT_ACCEPTED) {
        return refuse_document(refusal, out, out_len);
    }
    if (!contextual) {
        call->argument = root;
        return SPISOR_STATUS_SUCCESS;
    }
    int32_t status = split_envelope(root, call, out, out_len);
    cJSON_Delete(root);
    return status;
}

void spisor_discard_call(spisor_call *call) {
    cJSON_Delete(call->argument);
    cJSON_Delete(call->context);
    call->argument = NULL;
    call->context = NULL;
}

const char *spisor_context_string(const spisor_call *call, const char *fact) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(call->context, fact));
}

const char *spisor_environment(const spisor_call *call, const char *name) {
    const cJSON *environment = cJSON_GetObjectItemCaseSensitive(call->context, "environment");
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(environment, name));
}
