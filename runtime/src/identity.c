#include "identity.h"

#include <stdlib.h>
#include <string.h>

#include "segment.h"
#include "spisor/json_kind.h"

#define LOCAL_PREFIX "local:"

static int is_lowercase(char character) {
    return character >= 'a' && character <= 'z';
}

static int is_digit(char character) {
    return character >= '0' && character <= '9';
}

static int is_ascii_whitespace(char character) {
    return character == ' ' || (character >= '\t' && character <= '\r');
}

static int is_forge_character(char character) {
    return is_lowercase(character) || is_digit(character) || character == '-';
}

static int is_host_character(char character) {
    return is_lowercase(character) || is_digit(character) || character == '.' || character == '-';
}

static int is_segment_character(char character) {
    return is_lowercase(character) || is_digit(character) || character == '.' || character == '_' || character == '-';
}

static const char *run_of(const char *cursor, int (*accepts)(char)) {
    while (*cursor != '\0' && accepts(*cursor)) {
        cursor++;
    }
    return cursor;
}

static void set_part(runtime_identity_part *part, const char *start, const char *end) {
    part->start = start;
    part->length = (size_t)(end - start);
}

static int is_dot_segment(const char *start, size_t length) {
    return (length == 1 && start[0] == '.') || (length == 2 && start[0] == '.' && start[1] == '.');
}

static int is_valid_segment(const char *start, size_t length) {
    return length > 0 && !is_dot_segment(start, length) && run_of(start, is_segment_character) >= start + length;
}

static int is_valid_path(const char *path_start) {
    const char *cursor = path_start;
    while (*cursor == '/') {
        const char *segment = cursor + 1;
        const char *end = segment + strcspn(segment, "/");
        if (!is_valid_segment(segment, (size_t)(end - segment))) {
            return 0;
        }
        cursor = end;
    }
    return *cursor == '\0';
}

static int parse_local(const char *id, runtime_identity *parsed) {
    const char *name = id + strlen(LOCAL_PREFIX);
    const char *name_end = name;
    while (*name_end != '\0' && *name_end != '/' && !is_ascii_whitespace(*name_end)) {
        name_end++;
    }
    if (name_end == name || !is_valid_path(name_end)) {
        return 0;
    }
    set_part(&parsed->resolver, id, name - 1);
    set_part(&parsed->path, name, name + strlen(name));
    return 1;
}

static const char *parse_host(const char *cursor, runtime_identity *parsed) {
    if (*cursor != '@') {
        return cursor;
    }
    const char *end = run_of(cursor + 1, is_host_character);
    if (end == cursor + 1) {
        return NULL;
    }
    set_part(&parsed->host, cursor + 1, end);
    return end;
}

static int parse_forge(const char *id, runtime_identity *parsed) {
    if (!is_lowercase(*id)) {
        return 0;
    }
    const char *forge_end = run_of(id, is_forge_character);
    const char *after_host = parse_host(forge_end, parsed);
    if (after_host == NULL || *after_host != ':') {
        return 0;
    }
    const char *number_end = run_of(after_host + 1, is_digit);
    if (number_end == after_host + 1 || !is_valid_path(number_end)) {
        return 0;
    }
    set_part(&parsed->resolver, id, forge_end);
    set_part(&parsed->number, after_host + 1, number_end);
    if (*number_end == '/') {
        set_part(&parsed->path, number_end + 1, number_end + strlen(number_end));
    }
    return 1;
}

int runtime_identity_parse_id(const char *id, runtime_identity *parsed) {
    memset(parsed, 0, sizeof *parsed);
    if (strncmp(id, LOCAL_PREFIX, strlen(LOCAL_PREFIX)) == 0) {
        return parse_local(id, parsed);
    }
    return parse_forge(id, parsed);
}

static int attach_part(cJSON *answer, const char *name, runtime_identity_part part) {
    if (part.start == NULL) {
        return 1;
    }
    char *text = malloc(part.length + 1);
    if (text == NULL) {
        return 0;
    }
    memcpy(text, part.start, part.length);
    text[part.length] = '\0';
    int attached = cJSON_AddStringToObject(answer, name, text) != NULL;
    free(text);
    return attached;
}

static cJSON *parts_answer(const runtime_identity *parsed) {
    cJSON *answer = cJSON_CreateObject();
    if (answer == NULL || cJSON_AddTrueToObject(answer, "valid") == NULL || !attach_part(answer, "resolver", parsed->resolver) ||
        !attach_part(answer, "host", parsed->host) || !attach_part(answer, "number", parsed->number) ||
        !attach_part(answer, "path", parsed->path)) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static cJSON *invalid_answer(void) {
    cJSON *answer = cJSON_CreateObject();
    if (answer != NULL && cJSON_AddFalseToObject(answer, "valid") == NULL) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static int32_t answer(cJSON *result, uint8_t **out, size_t *out_len) {
    if (result == NULL) {
        *out = NULL;
        *out_len = 0;
        return SPISOR_STATUS_OUT_OF_MEMORY;
    }
    int32_t status = spisor_succeed(result, out, out_len);
    cJSON_Delete(result);
    return status;
}

static int32_t parse(const char *id, uint8_t **out, size_t *out_len) {
    runtime_identity parsed;
    return answer(runtime_identity_parse_id(id, &parsed) ? parts_answer(&parsed) : invalid_answer(), out, out_len);
}

static char *folder_name(const char *id) {
    size_t length = strlen(id);
    char *folder = malloc(length + 1);
    for (size_t index = 0; folder != NULL && index <= length; index++) {
        folder[index] = id[index] == ':' ? '+' : id[index] == '/' ? '=' : id[index];
    }
    return folder;
}

static int is_portable_folder(const char *folder) {
    char *portable = runtime_portable_segment(folder);
    int unchanged = portable != NULL && strcmp(portable, folder) == 0;
    free(portable);
    return unchanged;
}

static cJSON *folder_answer(const char *folder) {
    cJSON *result = cJSON_CreateObject();
    if (result != NULL && cJSON_AddStringToObject(result, "folder", folder) == NULL) {
        cJSON_Delete(result);
        result = NULL;
    }
    return result;
}

static int32_t folder(const char *id, uint8_t **out, size_t *out_len) {
    runtime_identity parsed;
    if (!runtime_identity_parse_id(id, &parsed)) {
        return spisor_fail("the id is not valid", "send an id such as github:<number>[/<path>] or local:<name>", out, out_len);
    }
    char *name = folder_name(id);
    if (name != NULL && !is_portable_folder(name)) {
        free(name);
        return spisor_fail("the id cannot name a folder", "use a local name without path separators, wildcards or a trailing dot", out, out_len);
    }
    int32_t status = answer(name == NULL ? NULL : folder_answer(name), out, out_len);
    free(name);
    return status;
}

static int32_t serve(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len,
                     int32_t (*operation)(const char *id, uint8_t **out, size_t *out_len)) {
    spisor_call call;
    int32_t status = spisor_parse_call(in, in_len, 0, &call, out, out_len);
    if (status != SPISOR_STATUS_SUCCESS) {
        return status;
    }
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(call.argument, "id");
    status = cJSON_IsString(id) ? operation(id->valuestring, out, out_len)
                                : spisor_fail("the argument carries no id", "send {\"id\": \"<id>\"}", out, out_len);
    spisor_discard_call(&call);
    return status;
}

RUNTIME_EXPORT int32_t runtime_identity_parse(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return serve(in, in_len, out, out_len, parse);
}

RUNTIME_EXPORT int32_t runtime_identity_folder(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return serve(in, in_len, out, out_len, folder);
}
