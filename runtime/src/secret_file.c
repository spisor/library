#include "secret_file.h"

#include <stdlib.h>
#include <string.h>

#include "spisor/file.h"
#include "spisor/json_file.h"
#include "spisor/json_write.h"

typedef int (*entry_matcher)(const char *entry, const char *pattern);

cJSON *runtime_read_secret_file(const char *path, runtime_secret_file_answer *answer) {
    spisor_json_file_state state;
    cJSON *entries = spisor_read_json_file(path, &state);
    if (state == SPISOR_JSON_FILE_ABSENT) {
        entries = cJSON_CreateObject();
        *answer = entries != NULL ? RUNTIME_SECRET_FILE_DONE : RUNTIME_SECRET_FILE_OUT_OF_MEMORY;
        return entries;
    }
    if (state == SPISOR_JSON_FILE_PARSED && cJSON_IsObject(entries)) {
        *answer = RUNTIME_SECRET_FILE_DONE;
        return entries;
    }
    cJSON_Delete(entries);
    *answer = RUNTIME_SECRET_FILE_UNREADABLE;
    return NULL;
}

static int is_separator(char character) {
    return character == '/' || character == '\\';
}

/** The directory part of path, freed with free; an empty string when path names a file at a root. */
static char *directory_of(const char *path) {
    size_t length = strlen(path);
    while (length > 0 && !is_separator(path[length - 1])) {
        length--;
    }
    size_t directory_length = length > 0 ? length - 1 : 0;
    char *directory = malloc(directory_length + 1);
    if (directory != NULL) {
        memcpy(directory, path, directory_length);
        directory[directory_length] = '\0';
    }
    return directory;
}

static int make_parent_directory(const char *path) {
    char *directory = directory_of(path);
    int made = directory != NULL && (directory[0] == '\0' || spisor_make_directories(directory));
    free(directory);
    return made;
}

static char *file_text(const cJSON *entries, size_t *length) {
    char *text = spisor_json_stringify_pretty(entries);
    if (text == NULL) {
        return NULL;
    }
    *length = strlen(text);
    char *terminated = realloc(text, *length + 2);
    if (terminated == NULL) {
        free(text);
        return NULL;
    }
    terminated[(*length)++] = '\n';
    terminated[*length] = '\0';
    return terminated;
}

runtime_secret_file_answer runtime_write_secret_file(const char *path, const cJSON *entries) {
    size_t length;
    char *text = file_text(entries, &length);
    if (text == NULL) {
        return RUNTIME_SECRET_FILE_OUT_OF_MEMORY;
    }
    int written = make_parent_directory(path) && spisor_replace_private_file(path, (const uint8_t *)text, length);
    free(text);
    if (written) {
        return RUNTIME_SECRET_FILE_DONE;
    }
    return spisor_is_link(path) ? RUNTIME_SECRET_FILE_LINKED : RUNTIME_SECRET_FILE_UNWRITABLE;
}

static char *copy_text(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy != NULL) {
        memcpy(copy, text, size);
    }
    return copy;
}

char *runtime_get_file_secret(const char *path, const char *entry, runtime_secret_file_answer *answer) {
    cJSON *entries = runtime_read_secret_file(path, answer);
    const char *held = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entries, entry));
    char *value = held != NULL ? copy_text(held) : NULL;
    if (held != NULL && value == NULL) {
        *answer = RUNTIME_SECRET_FILE_OUT_OF_MEMORY;
    }
    cJSON_Delete(entries);
    return value;
}

static int replace_entry(cJSON *entries, const char *entry, const char *value) {
    cJSON *item = cJSON_CreateString(value);
    if (item == NULL) {
        return 0;
    }
    int placed = cJSON_GetObjectItemCaseSensitive(entries, entry) != NULL
                     ? cJSON_ReplaceItemInObjectCaseSensitive(entries, entry, item)
                     : cJSON_AddItemToObject(entries, entry, item);
    if (!placed) {
        cJSON_Delete(item);
    }
    return placed;
}

runtime_secret_file_answer runtime_put_file_secret(const char *path, const char *entry, const char *value) {
    runtime_secret_file_answer answer;
    cJSON *entries = runtime_read_secret_file(path, &answer);
    if (entries == NULL) {
        return answer;
    }
    answer = replace_entry(entries, entry, value) ? runtime_write_secret_file(path, entries)
                                                  : RUNTIME_SECRET_FILE_OUT_OF_MEMORY;
    cJSON_Delete(entries);
    return answer;
}

static int is_entry(const char *entry, const char *pattern) {
    return strcmp(entry, pattern) == 0;
}

static int is_under(const char *entry, const char *prefix) {
    return strncmp(entry, prefix, strlen(prefix)) == 0;
}

static int remove_matching(cJSON *entries, entry_matcher matches, const char *pattern) {
    int removed = 0;
    cJSON *item = entries->child;
    while (item != NULL) {
        cJSON *next = item->next;
        if (matches(item->string, pattern)) {
            cJSON_Delete(cJSON_DetachItemViaPointer(entries, item));
            removed++;
        }
        item = next;
    }
    return removed;
}

static runtime_secret_file_answer remove_from_file(const char *path, entry_matcher matches, const char *pattern) {
    runtime_secret_file_answer answer;
    cJSON *entries = runtime_read_secret_file(path, &answer);
    if (entries == NULL) {
        return answer;
    }
    if (remove_matching(entries, matches, pattern) > 0) {
        answer = runtime_write_secret_file(path, entries);
    }
    cJSON_Delete(entries);
    return answer;
}

runtime_secret_file_answer runtime_remove_file_secret(const char *path, const char *entry) {
    return remove_from_file(path, is_entry, entry);
}

runtime_secret_file_answer runtime_remove_file_secrets_under(const char *path, const char *prefix) {
    return remove_from_file(path, is_under, prefix);
}
