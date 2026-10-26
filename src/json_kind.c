#include "spisor/json_kind.h"

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

static int is_json_whitespace(char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r';
}

static int only_whitespace_after(const char *end, const char *limit) {
    while (end < limit && is_json_whitespace(*end)) {
        end++;
    }
    return end == limit;
}

static cJSON *parse_whole(const uint8_t *in, size_t in_len) {
    const char *text = (const char *)in;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(text, in_len, &end, 0);
    if (root != NULL && !only_whitespace_after(end, text + in_len)) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

cJSON *spisor_last_member(const cJSON *object, const char *key) {
    cJSON *found = NULL;
    cJSON *member = NULL;
    if (!cJSON_IsObject(object)) {
        return NULL;
    }
    cJSON_ArrayForEach(member, object) {
        if (member->string != NULL && strcmp(member->string, key) == 0) {
            found = member;
        }
    }
    return found;
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
        if (spisor_last_member(context, rule->name) == fact && !rule->is_well_formed(fact)) {
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
    cJSON *argument = spisor_last_member(envelope, "argument");
    cJSON *context = spisor_last_member(envelope, "context");
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
    if (!is_valid_utf8(in, in_len)) {
        return spisor_fail("the input is not valid UTF-8", "send the argument as UTF-8 JSON", out, out_len);
    }
    cJSON *root = parse_whole(in, in_len);
    if (root == NULL) {
        return spisor_fail("the input is not valid JSON", "send exactly one JSON value", out, out_len);
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
    return cJSON_GetStringValue(spisor_last_member(call->context, fact));
}

const char *spisor_environment(const spisor_call *call, const char *name) {
    const cJSON *environment = spisor_last_member(call->context, "environment");
    return cJSON_GetStringValue(spisor_last_member(environment, name));
}
