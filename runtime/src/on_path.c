#include "on_path.h"

#include <stdlib.h>
#include <string.h>

#include "answer_storage.h"
#include "definition_support.h"
#include "directory_tree.h"
#include "followed_entry.h"
#include "paths.h"
#include "runtime/detect.h"
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
                           const char *wanted, int *out_of_memory) {
    for (size_t index = 0; index < listing->count; index++) {
        if (listing->entries[index].is_directory || !names_match(windows, listing->entries[index].name, wanted)) {
            continue;
        }
        char *path = joined(directory, listing->entries[index].name);
        if (path == NULL) {
            *out_of_memory = 1;
            return NULL;
        }
        if (runtime_is_followed_file(path, !windows)) {
            return path;
        }
        free(path);
    }
    return NULL;
}

static char *found_in(int windows, const char *directory, const accepted_names *accepted, int *out_of_memory) {
    runtime_directory_listing listing;
    if (!runtime_list_directory(directory, &listing)) {
        return NULL;
    }
    char *found = NULL;
    for (size_t index = 0; found == NULL && !*out_of_memory && index < accepted->count; index++) {
        found = matching_file(windows, directory, &listing, accepted->names[index], out_of_memory);
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
static char *found_on(int windows, const char *search, const accepted_names *accepted, int *out_of_memory) {
    char separator = windows || RUNTIME_HOST_IS_WINDOWS ? ';' : ':';
    char *found = NULL;
    while (found == NULL && !*out_of_memory && *search != '\0') {
        const char *end = strchr(search, separator);
        size_t length = end == NULL ? strlen(search) : (size_t)(end - search);
        char *directory = length == 0 ? NULL : directory_of(search, length);
        *out_of_memory = length != 0 && directory == NULL;
        found = directory == NULL ? NULL : found_in(windows, directory, accepted, out_of_memory);
        free(directory);
        search = end == NULL ? search + length : end + 1;
    }
    return found;
}

runtime_on_path_status runtime_find_on_path(int windows, const char *binary, const cJSON *environment,
                                            char **found) {
    const char *search = runtime_environment_variable(environment, "PATH", windows);
    accepted_names accepted = {NULL, 0};
    int out_of_memory = 0;
    int named = windows ? windows_names(binary, environment, &accepted) : add_name(&accepted, binary, "", 0);
    *found = named && search != NULL ? found_on(windows, search, &accepted, &out_of_memory) : NULL;
    free_names(&accepted);
    if (!named || out_of_memory) {
        return RUNTIME_ON_PATH_OUT_OF_MEMORY;
    }
    return *found == NULL ? RUNTIME_ON_PATH_ABSENT : RUNTIME_ON_PATH_FOUND;
}

#define ARGUMENT_SHAPE RUNTIME_SHAPE_THROUGH(runtime_detect_on_path_argument, environment_count)
#define ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_detect_on_path_answer, storage)

static spisor_status answer_found(char *found, runtime_detect_on_path_answer *answer) {
    int copied = runtime_storage_copy(&answer->storage, found, &answer->path);
    free(found);
    if (!copied) {
        runtime_release_stored_answer(answer, ANSWER_SHAPE);
        return SPISOR_OUT_OF_MEMORY;
    }
    return SPISOR_OK;
}

static spisor_status search_path(const char *binary, const cJSON *environment, runtime_detect_on_path_answer *answer) {
    char *found = NULL;
    runtime_on_path_status searched = runtime_find_on_path(RUNTIME_HOST_IS_WINDOWS, binary, environment, &found);
    if (searched == RUNTIME_ON_PATH_OUT_OF_MEMORY) {
        return SPISOR_OUT_OF_MEMORY;
    }
    return searched == RUNTIME_ON_PATH_ABSENT ? SPISOR_NONE : answer_found(found, answer);
}

static spisor_status on_path(void *context, const runtime_detect_on_path_argument *argument,
                             runtime_detect_on_path_answer *answer, spisor_refusal *refusal) {
    cJSON *environment = NULL;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, ARGUMENT_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "detect's on_path");
    }
    if (argument->binary == NULL || argument->binary[0] == '\0') {
        return runtime_refuse_as(refusal, SPISOR_MALFORMED, "the argument carries no binary",
                                 "send binary as a command name, such as \"claude\"");
    }
    status = runtime_environment_json(argument->environment, argument->environment_count, &environment);
    if (status == SPISOR_MALFORMED) {
        return runtime_refuse_environment(refusal);
    }
    if (status == SPISOR_OK) {
        status = search_path(argument->binary, environment, answer);
    }
    cJSON_Delete(environment);
    return status;
}

static void release_answer(void *context, runtime_detect_on_path_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, ANSWER_SHAPE);
}

static const runtime_detect_definition definition = {
    sizeof(runtime_detect_definition), NULL, on_path, release_answer, runtime_release_refusal,
};

RUNTIME_DEFINITION_API const void *runtime_detect_definition_of(void) {
    return &definition;
}
