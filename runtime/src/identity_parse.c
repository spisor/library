#include "identity.h"

#include <stdlib.h>
#include <string.h>

#include "segment.h"

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

/** Whether a local id's name holds a character the folder mapping or a case-insensitive file system would make ambiguous. */
static int is_ambiguous_local_name(const runtime_identity *parsed) {
    if (parsed->number.start != NULL) {
        return 0;
    }
    for (size_t index = 0; index < parsed->path.length; index++) {
        char character = parsed->path.start[index];
        if (character == '+' || character == '=' || character == ':' || (character >= 'A' && character <= 'Z')) {
            return 1;
        }
    }
    return 0;
}

runtime_identity_folder_status runtime_identity_folder_name(const char *id, char **folder) {
    runtime_identity parsed;
    *folder = NULL;
    if (!runtime_identity_parse_id(id, &parsed)) {
        return RUNTIME_IDENTITY_FOLDER_INVALID;
    }
    if (is_ambiguous_local_name(&parsed)) {
        return RUNTIME_IDENTITY_FOLDER_AMBIGUOUS;
    }
    char *name = folder_name(id);
    if (name == NULL) {
        return RUNTIME_IDENTITY_FOLDER_OUT_OF_MEMORY;
    }
    if (!is_portable_folder(name)) {
        free(name);
        return RUNTIME_IDENTITY_FOLDER_NOT_PORTABLE;
    }
    *folder = name;
    return RUNTIME_IDENTITY_FOLDER_NAMED;
}


static char *copy_text(const char *text) {
    char *copy = malloc(strlen(text) + 1);
    if (copy != NULL) {
        strcpy(copy, text);
    }
    return copy;
}

char *runtime_item_folder(const char *key, int *unfit) {
    runtime_identity parsed;
    *unfit = 0;
    if (!runtime_identity_parse_id(key, &parsed)) {
        return copy_text(key);
    }
    char *folder = NULL;
    runtime_identity_folder_status status = runtime_identity_folder_name(key, &folder);
    *unfit = status != RUNTIME_IDENTITY_FOLDER_NAMED && status != RUNTIME_IDENTITY_FOLDER_OUT_OF_MEMORY;
    return folder;
}

char *runtime_item_key_of_folder(const char *folder) {
    char *key = copy_text(folder);
    for (char *at = key; at != NULL && *at != '\0'; at++) {
        *at = *at == '+' ? ':' : *at == '=' ? '/' : *at;
    }
    runtime_identity parsed;
    char *round_trip = NULL;
    int is_id = key != NULL && runtime_identity_parse_id(key, &parsed) &&
                runtime_identity_folder_name(key, &round_trip) == RUNTIME_IDENTITY_FOLDER_NAMED &&
                strcmp(round_trip, folder) == 0;
    free(round_trip);
    if (key != NULL && !is_id) {
        strcpy(key, folder);
    }
    return key;
}
