#include "on_path.h"

#include <stdlib.h>
#include <string.h>

#include "catalog_answer.h"
#include "directory_tree.h"
#include "followed_entry.h"
#include "json_service.h"
#include "paths.h"
#include "spisor/json_kind.h"
#include "spisor/path.h"
#include "user_home.h"

#define DEFAULT_PATH_EXTENSIONS ".COM;.EXE;.BAT;.CMD"

typedef struct accepted_names {
    char **names;
    size_t count;
} accepted_names;

static void free_names(accepted_names *accepted) {
    for (size_t index = 0; index < accepted->count; index++) {
        free(accepted->names[index]);
    }
    free(accepted->names);
}

static int add_name(accepted_names *accepted, const char *binary, const char *extension, size_t extension_length) {
    size_t length = strlen(binary);
    char *name = malloc(length + extension_length + 1);
    char **grown = name == NULL ? NULL : realloc(accepted->names, (accepted->count + 1) * sizeof *grown);
    if (grown == NULL) {
        free(name);
        return 0;
    }
    memcpy(name, binary, length);
    memcpy(name + length, extension, extension_length);
    name[length + extension_length] = '\0';
    accepted->names = grown;
    accepted->names[accepted->count++] = name;
    return 1;
}

static int windows_names(const char *binary, const cJSON *environment, accepted_names *accepted) {
    const char *extensions = runtime_environment_variable(environment, "PATHEXT", 1);
    extensions = extensions == NULL || extensions[0] == '\0' ? DEFAULT_PATH_EXTENSIONS : extensions;
    int complete = 1;
    while (complete && *extensions != '\0') {
        size_t length = strcspn(extensions, ";");
        complete = length == 0 || add_name(accepted, binary, extensions, length);
        extensions += length + (extensions[length] == ';');
    }
    return complete && add_name(accepted, binary, "", 0);
}

static int names_match(int windows, const char *entry, const char *wanted) {
    return windows ? runtime_equal_ignoring_ascii_case(entry, wanted) : strcmp(entry, wanted) == 0;
}

static char *joined(const char *directory, const char *name) {
    const char *segments[2] = {directory, name};
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
}

static char *matching_file(int windows, const char *directory, const runtime_directory_listing *listing,
                           const char *wanted) {
    for (size_t index = 0; index < listing->count; index++) {
        if (listing->entries[index].is_directory || !names_match(windows, listing->entries[index].name, wanted)) {
            continue;
        }
        char *path = joined(directory, listing->entries[index].name);
        if (path == NULL || runtime_is_followed_file(path, !windows)) {
            return path;
        }
        free(path);
    }
    return NULL;
}

static char *found_in(int windows, const char *directory, const accepted_names *accepted) {
    runtime_directory_listing listing;
    if (!runtime_list_directory(directory, &listing)) {
        return NULL;
    }
    char *found = NULL;
    for (size_t index = 0; found == NULL && index < accepted->count; index++) {
        found = matching_file(windows, directory, &listing, accepted->names[index]);
    }
    runtime_free_directory_listing(&listing);
    return found;
}

static char *directory_of(const char *start, size_t length) {
    char *directory = malloc(length + 1);
    if (directory != NULL) {
        memcpy(directory, start, length);
        directory[length] = '\0';
    }
    return directory;
}

/** @implNote The separator is ";" whenever the rules or this OS are Windows, as Node's path.delimiter is. */
static char *found_on(int windows, const char *search, const accepted_names *accepted) {
    char separator = windows || RUNTIME_HOST_IS_WINDOWS ? ';' : ':';
    char *found = NULL;
    while (found == NULL && *search != '\0') {
        const char *end = strchr(search, separator);
        size_t length = end == NULL ? strlen(search) : (size_t)(end - search);
        char *directory = length == 0 ? NULL : directory_of(search, length);
        found = directory == NULL ? NULL : found_in(windows, directory, accepted);
        free(directory);
        search = end == NULL ? search + length : end + 1;
    }
    return found;
}

char *runtime_find_on_path(int windows, const char *binary, const cJSON *environment) {
    const char *search = runtime_environment_variable(environment, "PATH", windows);
    accepted_names accepted = {NULL, 0};
    int named = windows ? windows_names(binary, environment, &accepted) : add_name(&accepted, binary, "", 0);
    char *found = named && search != NULL ? found_on(windows, search, &accepted) : NULL;
    free_names(&accepted);
    return found;
}

static cJSON *on_path_for(const cJSON *argument, cJSON **refusal) {
    const char *binary = runtime_catalog_text_member(argument, "binary");
    const cJSON *environment = cJSON_GetObjectItemCaseSensitive(argument, "environment");
    if (binary == NULL || binary[0] == '\0' || !cJSON_IsObject(environment)) {
        *refusal = runtime_refusal("the argument carries no usable binary or environment",
                                   "send {\"binary\": \"<command name>\", \"environment\": {<name>: <value>}}");
        return NULL;
    }
    cJSON *answer = cJSON_CreateObject();
    char *found = answer == NULL ? NULL : runtime_find_on_path(RUNTIME_HOST_IS_WINDOWS, binary, environment);
    if (found != NULL && cJSON_AddStringToObject(answer, "path", found) == NULL) {
        cJSON_Delete(answer);
        answer = NULL;
    }
    free(found);
    return answer;
}

RUNTIME_EXPORT int32_t runtime_detect_on_path(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, on_path_for);
}
