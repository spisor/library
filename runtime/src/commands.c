#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#include "changes.h"
#include "directory_tree.h"
#include "hosts_lock.h"
#include "paths.h"
#include "registry.h"
#include "shim_format.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"
#include "spisor/path.h"

#define COMMAND_NAME_LIMIT 64
#define NAME_BUFFER_SIZE 80
#define FALLBACK_LIMIT 999
#define REGISTRATION_SUFFIX ".json"
#define REGISTRATION_SUFFIX_LENGTH 5
#define DEFAULT_PATH_EXTENSIONS ".COM;.EXE;.BAT;.CMD"

#if defined(_WIN32)
#define PATH_LIST_SEPARATOR ';'
#define SHIM_FILE_COUNT 2
#define LAUNCHER_FILE_NAME L"runtime-launcher.exe"
#else
#define PATH_LIST_SEPARATOR ':'
#define SHIM_FILE_COUNT 1
#endif

typedef struct command_call {
    const char *state;
    const char *host;
    const cJSON *argument;
    char *shims;
    cJSON *registration;
    char name[NAME_BUFFER_SIZE];
    int answers_name;
    int answers_deleted;
    char reason[1400];
    const char *fix;
} command_call;

typedef int (*command_step)(command_call *call);

static const char *text_of(const cJSON *object, const char *member) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, member));
}

static int refuse(command_call *call, const char *fix, const char *format, const char *subject) {
    snprintf(call->reason, sizeof call->reason, format, subject);
    call->fix = fix;
    return 0;
}

static int out_of_memory(command_call *call) {
    return refuse(call, "try again", "%s", "memory ran out");
}

static char folded(char character) {
    return RUNTIME_HOST_IS_WINDOWS && character >= 'A' && character <= 'Z' ? (char)(character - 'A' + 'a') : character;
}

/** Whether two command names are one: equal, ignoring ASCII case on Windows, where file names do. */
static int names_equal(const char *left, const char *right) {
    if (left == NULL || right == NULL) {
        return 0;
    }
    while (*left != '\0' && folded(*left) == folded(*right)) {
        left++;
        right++;
    }
    return folded(*left) == folded(*right);
}

static int is_name_character(char character, int first) {
    int alphanumeric = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                       (character >= '0' && character <= '9');
    return alphanumeric || (!first && (character == '-' || character == '_' || character == '.'));
}

/** A name that is one portable file name on every OS: a letter or digit, then letters, digits, - _ or . */
static int is_command_name(const char *name) {
    size_t length = name == NULL ? 0 : strlen(name);
    if (length == 0 || length > COMMAND_NAME_LIMIT) {
        return 0;
    }
    for (size_t index = 0; index < length; index++) {
        if (!is_name_character(name[index], index == 0)) {
            return 0;
        }
    }
    return 1;
}

static const char *target_defect(const char *target) {
    if (target == NULL || !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, target)) {
        return "the command's target must be an absolute path";
    }
    if (strpbrk(target, "\r\n") != NULL) {
        return "the command's target holds a line break";
    }
    return NULL;
}

/** NULL when arguments is absent or an array of strings with no line break, else what is wrong with it. */
static const char *arguments_defect(const cJSON *arguments) {
    if (arguments == NULL) {
        return NULL;
    }
    if (!cJSON_IsArray(arguments)) {
        return "the command's arguments must be an array of strings";
    }
    const cJSON *argument;
    cJSON_ArrayForEach(argument, arguments) {
        if (!cJSON_IsString(argument)) {
            return "the command's arguments must be an array of strings";
        }
        if (strpbrk(argument->valuestring, "\r\n") != NULL) {
            return "an argument of the command holds a line break";
        }
    }
    return NULL;
}

static char *joined(const char *directory, const char *name) {
    const char *segments[2] = {directory, name};
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
}

static cJSON *change_of(const char *kind, const char *member, const char *value) {
    cJSON *change = cJSON_CreateObject();
    if (change == NULL || cJSON_AddStringToObject(change, "kind", kind) == NULL ||
        cJSON_AddStringToObject(change, member, value) == NULL) {
        cJSON_Delete(change);
        return NULL;
    }
    return change;
}

/**
 * The index-th file of the shim of name: on Windows <name>.exe, the launcher, then <name>.shim, the
 * target it runs; on POSIX the one script <name>.
 */
static char *shim_file(const char *shims, const char *name, size_t index) {
    char file_name[NAME_BUFFER_SIZE + 8];
    if (RUNTIME_HOST_IS_WINDOWS) {
        snprintf(file_name, sizeof file_name, index == 0 ? "%s.exe" : "%s.shim", name);
    } else {
        snprintf(file_name, sizeof file_name, "%s", name);
    }
    return joined(shims, file_name);
}

#if defined(_WIN32)
static char *utf8_of(const wchar_t *wide) {
    int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    char *text = size > 0 ? malloc((size_t)size) : NULL;
    if (text != NULL && WideCharToMultiByte(CP_UTF8, 0, wide, -1, text, size, NULL, NULL) != size) {
        free(text);
        text = NULL;
    }
    return text;
}

/**
 * The launcher every Windows shim is a copy of, runtime-launcher.exe in the directory of the module
 * holding this code: runtime.dll, or a test executable that links the runtime objects.
 *
 * @implNote Found beside the library rather than passed by the caller because the launcher is built
 * and shipped with the library, so the library is the one thing that knows where it is; a passed path
 * would make every host carry the location of a build artefact. This reads the module's own location,
 * never the user's environment.
 */
static wchar_t *module_path(HMODULE module, size_t *capacity) {
    for (*capacity = MAX_PATH; *capacity <= 32768; *capacity *= 2) {
        wchar_t *path = malloc(*capacity * sizeof(wchar_t));
        DWORD length = path == NULL ? 0 : GetModuleFileNameW(module, path, (DWORD)*capacity);
        if (length > 0 && length < *capacity) {
            return path;
        }
        free(path);
        if (length == 0) {
            return NULL;
        }
    }
    return NULL;
}

static char *launcher_path(void) {
    HMODULE module = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(void *)launcher_path, &module)) {
        return NULL;
    }
    size_t capacity = 0;
    wchar_t *path = module_path(module, &capacity);
    wchar_t *slash = path == NULL ? NULL : wcsrchr(path, L'\\');
    size_t name_offset = slash == NULL ? 0 : (size_t)(slash + 1 - path);
    size_t needed = name_offset + wcslen(LAUNCHER_FILE_NAME) + 1;
    wchar_t *launcher = slash == NULL ? NULL : realloc(path, (needed > capacity ? needed : capacity) * sizeof(wchar_t));
    if (launcher == NULL) {
        free(path);
        return NULL;
    }
    wcscpy_s(launcher + name_offset, needed - name_offset, LAUNCHER_FILE_NAME);
    char *text = utf8_of(launcher);
    free(launcher);
    return text;
}

static char *backslashed(const char *path) {
    size_t length = strlen(path);
    char *copy = malloc(length + 1);
    if (copy != NULL) {
        for (size_t index = 0; index <= length; index++) {
            copy[index] = path[index] == '/' ? '\\' : path[index];
        }
    }
    return copy;
}

static size_t append_line(char *content, size_t used, const char *line) {
    size_t length = strlen(line);
    memcpy(content + used, line, length);
    content[used + length] = '\n';
    return used + length + 1;
}

/** The .shim file: the format line, the target with backslashes, then each fixed argument, one per line. */
static char *shim_text(const char *target, const cJSON *arguments) {
    size_t size = strlen(RUNTIME_SHIM_FORMAT) + strlen(target) + 3;
    const cJSON *argument;
    cJSON_ArrayForEach(argument, arguments) {
        size += strlen(argument->valuestring) + 1;
    }
    char *content = malloc(size);
    char *windows_target = backslashed(target);
    if (content == NULL || windows_target == NULL) {
        free(content);
        free(windows_target);
        return NULL;
    }
    size_t used = append_line(content, 0, RUNTIME_SHIM_FORMAT);
    used = append_line(content, used, windows_target);
    cJSON_ArrayForEach(argument, arguments) {
        used = append_line(content, used, argument->valuestring);
    }
    content[used] = '\0';
    free(windows_target);
    return content;
}

static cJSON *shim_change(const char *path, size_t index, const char *target, const cJSON *arguments) {
    cJSON *change = path == NULL ? NULL : change_of("file", "path", path);
    char *launcher = index == 0 ? launcher_path() : NULL;
    char *content = index == 0 ? NULL : shim_text(target, arguments);
    int complete = change != NULL &&
                   (index == 0 ? launcher != NULL && cJSON_AddStringToObject(change, "source", launcher) != NULL
                               : content != NULL && cJSON_AddStringToObject(change, "content", content) != NULL) &&
                   cJSON_AddTrueToObject(change, "executable") != NULL;
    free(launcher);
    free(content);
    if (!complete) {
        cJSON_Delete(change);
        return NULL;
    }
    return change;
}
#else
static size_t quoted_size(const char *word) {
    size_t size = strlen(word) + 3;
    for (const char *cursor = word; *cursor != '\0'; cursor++) {
        size += *cursor == '\'' ? 3 : 0;
    }
    return size;
}

/** Appends word single-quoted and preceded by a space; a ' in it is written as '\''. */
static size_t append_quoted(char *content, size_t used, const char *word) {
    content[used++] = ' ';
    content[used++] = '\'';
    for (const char *cursor = word; *cursor != '\0'; cursor++) {
        if (*cursor == '\'') {
            memcpy(content + used, "'\\''", 4);
            used += 4;
        } else {
            content[used++] = *cursor;
        }
    }
    content[used++] = '\'';
    return used;
}

/** An sh script that execs target and each fixed argument, every one single-quoted, then "$@". */
static char *shim_content(const char *target, const cJSON *arguments) {
    static const char opening[] = "#!/bin/sh\nexec";
    static const char closing[] = " \"$@\"\n";
    size_t size = sizeof opening + sizeof closing + quoted_size(target);
    const cJSON *argument;
    cJSON_ArrayForEach(argument, arguments) {
        size += quoted_size(argument->valuestring);
    }
    char *content = malloc(size);
    if (content == NULL) {
        return NULL;
    }
    memcpy(content, opening, sizeof opening - 1);
    size_t used = append_quoted(content, sizeof opening - 1, target);
    cJSON_ArrayForEach(argument, arguments) {
        used = append_quoted(content, used, argument->valuestring);
    }
    memcpy(content + used, closing, sizeof closing);
    return content;
}

static cJSON *shim_change(const char *path, size_t index, const char *target, const cJSON *arguments) {
    (void)index;
    cJSON *change = path == NULL ? NULL : change_of("file", "path", path);
    char *content = shim_content(target, arguments);
    int complete = change != NULL && content != NULL &&
                   cJSON_AddStringToObject(change, "content", content) != NULL &&
                   cJSON_AddTrueToObject(change, "executable") != NULL;
    free(content);
    if (!complete) {
        cJSON_Delete(change);
        return NULL;
    }
    return change;
}
#endif

static const char *environment_value(const cJSON *environment, const char *name) {
    const cJSON *entry;
    cJSON_ArrayForEach(entry, environment) {
        int matches = RUNTIME_HOST_IS_WINDOWS ? names_equal(entry->string, name) : strcmp(entry->string, name) == 0;
        if (matches && cJSON_IsString(entry)) {
            return entry->valuestring;
        }
    }
    return NULL;
}

#if defined(_WIN32)
static int is_file_entry(const char *path) {
    runtime_entry_kind found = runtime_probe_entry(path);
    return found == RUNTIME_ENTRY_OTHER || found == RUNTIME_ENTRY_UNKNOWN;
}

static int ends_with_extension(const char *name, const char *extension, size_t extension_length) {
    size_t length = strlen(name);
    if (extension_length == 0 || length <= extension_length) {
        return 0;
    }
    for (size_t index = 0; index < extension_length; index++) {
        if (folded(name[length - extension_length + index]) != folded(extension[index])) {
            return 0;
        }
    }
    return 1;
}

static int has_extension_file(const char *directory, const char *name, const char *extension, size_t length) {
    char file_name[NAME_BUFFER_SIZE + 64];
    if (length == 0 || length > 32) {
        return 0;
    }
    snprintf(file_name, sizeof file_name, "%s%.*s", name, (int)length, extension);
    char *path = joined(directory, file_name);
    int found = path != NULL && is_file_entry(path);
    free(path);
    return found;
}

static int is_held_in(const char *directory, const char *name, const char *extensions) {
    int named_with_extension = 0;
    const char *start = extensions;
    while (*start != '\0') {
        const char *end = strchr(start, ';');
        size_t length = end == NULL ? strlen(start) : (size_t)(end - start);
        if (has_extension_file(directory, name, start, length)) {
            return 1;
        }
        named_with_extension |= ends_with_extension(name, start, length);
        start = end == NULL ? start + length : end + 1;
    }
    char *bare = named_with_extension ? joined(directory, name) : NULL;
    int found = bare != NULL && is_file_entry(bare);
    free(bare);
    return found;
}
#else
static int is_held_in(const char *directory, const char *name, const char *extensions) {
    (void)extensions;
    char *path = joined(directory, name);
    struct stat status;
    int found = path != NULL && stat(path, &status) == 0 && S_ISREG(status.st_mode) &&
                (status.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
    free(path);
    return found;
}
#endif

/** One PATH entry, its surrounding quotes dropped on Windows; NULL when it is empty or not absolute. */
static char *path_directory(const char *start, size_t length) {
    if (RUNTIME_HOST_IS_WINDOWS && length >= 2 && start[0] == '"' && start[length - 1] == '"') {
        start++;
        length -= 2;
    }
    char *directory = malloc(length + 1);
    if (directory == NULL) {
        return NULL;
    }
    memcpy(directory, start, length);
    directory[length] = '\0';
    if (length == 0 || !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, directory)) {
        free(directory);
        return NULL;
    }
    return directory;
}

int runtime_commands_held_on_path(const cJSON *environment, const char *shims, const char *name) {
    const char *search = environment_value(environment, "PATH");
    const char *extensions = environment_value(environment, "PATHEXT");
    extensions = extensions == NULL || extensions[0] == '\0' ? DEFAULT_PATH_EXTENSIONS : extensions;
    int held = 0;
    const char *start = search == NULL ? "" : search;
    while (!held && *start != '\0') {
        const char *end = strchr(start, PATH_LIST_SEPARATOR);
        size_t length = end == NULL ? strlen(start) : (size_t)(end - start);
        char *directory = path_directory(start, length);
        held = directory != NULL && !runtime_change_paths_equal(directory, shims) &&
               is_held_in(directory, name, extensions);
        free(directory);
        start = end == NULL ? start + length : end + 1;
    }
    return held;
}

static cJSON *commands_of(const cJSON *registration) {
    return cJSON_GetObjectItemCaseSensitive(registration, "commands");
}

static int is_active(const cJSON *entry) {
    const char *state = text_of(entry, "state");
    return state != NULL && strcmp(state, "active") == 0;
}

static cJSON *active_named(const cJSON *registration, const char *name) {
    cJSON *entry;
    cJSON_ArrayForEach(entry, commands_of(registration)) {
        if (is_active(entry) && names_equal(text_of(entry, "name"), name)) {
            return entry;
        }
    }
    return NULL;
}

static int active_count(const cJSON *registration) {
    int count = 0;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, commands_of(registration)) {
        count += is_active(entry);
    }
    return count;
}

/** The entry for requested, an active one first; NULL when the host never asked for it. */
static cJSON *entry_for_requested(const cJSON *registration, const char *requested) {
    cJSON *deleted = NULL;
    cJSON *entry;
    cJSON_ArrayForEach(entry, commands_of(registration)) {
        if (!names_equal(text_of(entry, "requested"), requested)) {
            continue;
        }
        if (is_active(entry)) {
            return entry;
        }
        deleted = deleted == NULL ? entry : deleted;
    }
    return deleted;
}

static int is_registration_file(const runtime_directory_entry *entry) {
    size_t length = strlen(entry->name);
    return !entry->is_directory && length > REGISTRATION_SUFFIX_LENGTH &&
           strcmp(entry->name + length - REGISTRATION_SUFFIX_LENGTH, REGISTRATION_SUFFIX) == 0;
}

static int registration_holds(const char *state, const char *file_name, const char *name) {
    char id[512];
    size_t id_length = strlen(file_name) - REGISTRATION_SUFFIX_LENGTH;
    if (id_length >= sizeof id) {
        return 0;
    }
    memcpy(id, file_name, id_length);
    id[id_length] = '\0';
    cJSON *registration = NULL;
    int holds = runtime_registry_load(state, id, &registration) == RUNTIME_REGISTRY_OK &&
                active_named(registration, name) != NULL;
    cJSON_Delete(registration);
    return holds;
}

/**
 * @implNote A registration that cannot be read is passed over here; its shims still stand in
 * state/bin, which is_name_held checks on the disk, so its names stay held.
 */
static int is_held_by_a_command(const char *state, const char *name) {
    char *directory = joined(state, "hosts");
    runtime_directory_listing listing;
    if (directory == NULL || !runtime_list_directory(directory, &listing)) {
        free(directory);
        return 0;
    }
    free(directory);
    int held = 0;
    for (size_t index = 0; index < listing.count && !held; index++) {
        held = is_registration_file(&listing.entries[index]) &&
               registration_holds(state, listing.entries[index].name, name);
    }
    runtime_free_directory_listing(&listing);
    return held;
}

static int is_shim_taken(const command_call *call, const char *name) {
    int taken = 0;
    for (size_t index = 0; index < SHIM_FILE_COUNT && !taken; index++) {
        char *path = shim_file(call->shims, name, index);
        taken = path == NULL || runtime_probe_entry(path) != RUNTIME_ENTRY_MISSING;
        free(path);
    }
    return taken;
}

static int is_name_held(const command_call *call, const char *name) {
    return is_held_by_a_command(call->state, name) || is_shim_taken(call, name) ||
           runtime_commands_held_on_path(cJSON_GetObjectItemCaseSensitive(call->argument, "environment"),
                                         call->shims, name);
}

/** The requested name when it is free, else the first free of <name>2, <name>3 and so on. */
static int assign_name(command_call *call, const char *requested) {
    for (int attempt = 1; attempt <= FALLBACK_LIMIT; attempt++) {
        if (attempt == 1) {
            snprintf(call->name, sizeof call->name, "%s", requested);
        } else {
            snprintf(call->name, sizeof call->name, "%s%d", requested, attempt);
        }
        if (!is_name_held(call, call->name)) {
            return 1;
        }
    }
    return refuse(call, "rename or delete some of the commands holding it", "every fallback of %s is held",
                  requested);
}

static int apply_change(command_call *call, cJSON *change) {
    if (change == NULL) {
        return out_of_memory(call);
    }
    runtime_changes_outcome outcome = runtime_changes_apply_held(call->state, call->host, change);
    cJSON_Delete(change);
    if (outcome.answer == RUNTIME_CHANGES_RECORDED || outcome.answer == RUNTIME_CHANGES_UNNEEDED) {
        return 1;
    }
    return refuse(call, "fix the cause and ask for the command again", "%s", outcome.reason);
}

static cJSON *path_entry_change(const command_call *call, const char *home) {
    cJSON *change = change_of("path-entry", "directory", call->shims);
    const char *shell = environment_value(cJSON_GetObjectItemCaseSensitive(call->argument, "environment"), "SHELL");
    if (change != NULL && (cJSON_AddStringToObject(change, "home", home) == NULL ||
                           (shell != NULL && cJSON_AddStringToObject(change, "shell", shell) == NULL))) {
        cJSON_Delete(change);
        return NULL;
    }
    return change;
}

static int write_shim(command_call *call, const char *name) {
    const char *target = text_of(call->argument, "target");
    const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(call->argument, "arguments");
    int written = 1;
    for (size_t index = 0; index < SHIM_FILE_COUNT && written; index++) {
        char *path = shim_file(call->shims, name, index);
        written = apply_change(call, shim_change(path, index, target, arguments));
        free(path);
    }
    return written;
}

/** Records state/bin, its PATH entry and the shim itself, each a no-op when this host already has it. */
static int make_shim(command_call *call, const char *name) {
    const char *home = text_of(call->argument, "home");
    if (home == NULL || !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, home)) {
        return refuse(call, "send the user's home as an absolute path", "%s", "the call carries no usable home");
    }
    return apply_change(call, change_of("directory", "path", call->shims)) &&
           apply_change(call, path_entry_change(call, home)) && write_shim(call, name);
}

static int load_registration(command_call *call) {
    cJSON_Delete(call->registration);
    call->registration = NULL;
    runtime_registry_answer loaded = runtime_registry_load(call->state, call->host, &call->registration);
    if (loaded == RUNTIME_REGISTRY_ABSENT) {
        return refuse(call, "register the host first", "the host %s is not registered", call->host);
    }
    if (loaded != RUNTIME_REGISTRY_OK) {
        return refuse(call, "repair or restore it", "the registration of host %s could not be read", call->host);
    }
    if (!cJSON_IsArray(commands_of(call->registration)) &&
        cJSON_AddArrayToObject(call->registration, "commands") == NULL) {
        return out_of_memory(call);
    }
    return 1;
}

static int store_registration(command_call *call) {
    if (runtime_registry_store(call->state, call->registration) == RUNTIME_REGISTRY_OK) {
        return 1;
    }
    return refuse(call, "make the registry writable and try again", "the registration of host %s could not be written",
                  call->host);
}

/** Appends an active entry; requested is NULL for a command the user added, which no host asked for. */
static int append_entry(command_call *call, const char *requested, const char *owner) {
    cJSON *entry = cJSON_CreateObject();
    int complete = entry != NULL && cJSON_AddStringToObject(entry, "name", call->name) != NULL &&
                   (requested == NULL ? cJSON_AddNullToObject(entry, "requested") != NULL
                                      : cJSON_AddStringToObject(entry, "requested", requested) != NULL) &&
                   cJSON_AddStringToObject(entry, "owner", owner) != NULL &&
                   cJSON_AddStringToObject(entry, "state", "active") != NULL &&
                   cJSON_AddItemToArray(commands_of(call->registration), entry);
    if (!complete) {
        cJSON_Delete(entry);
        return out_of_memory(call);
    }
    return 1;
}

static int set_text(cJSON *object, const char *member, const char *text) {
    cJSON *value = cJSON_CreateString(text);
    int set = value != NULL && (cJSON_GetObjectItemCaseSensitive(object, member) != NULL
                                    ? cJSON_ReplaceItemInObjectCaseSensitive(object, member, value)
                                    : cJSON_AddItemToObject(object, member, value));
    if (value != NULL && !set) {
        cJSON_Delete(value);
    }
    return set;
}

static cJSON *shim_record(const command_call *call, const char *name, size_t index) {
    char *path = shim_file(call->shims, name, index);
    cJSON *found = NULL;
    cJSON *entry;
    cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(call->registration, "changes")) {
        const char *kind = text_of(entry, "kind");
        if (found == NULL && kind != NULL && strcmp(kind, "file") == 0 &&
            !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "reversed")) &&
            runtime_change_paths_equal(text_of(entry, "path"), path)) {
            found = entry;
        }
    }
    free(path);
    return found;
}

static int remove_shim_file(command_call *call, cJSON *record) {
    if (record == NULL) {
        return 1;
    }
    runtime_change_note note = {""};
    int failed = 0;
    runtime_change_reversal reversal =
        runtime_changes_reverse_entry(call->state, call->host, call->registration, record, &note, &failed);
    if (failed) {
        return out_of_memory(call);
    }
    if (reversal == RUNTIME_CHANGE_NOT_REVERSED) {
        return refuse(call, "fix the cause and try again", "the shim could not be removed: %s", note.detail);
    }
    return 1;
}

/** Reverses the recorded shim files of name in the loaded registration; a file with no record is left. */
static int remove_shim(command_call *call, const char *name) {
    for (size_t index = 0; index < SHIM_FILE_COUNT; index++) {
        if (!remove_shim_file(call, shim_record(call, name, index))) {
            return 0;
        }
    }
    return 1;
}

static int validated_name(command_call *call, const char *member) {
    const char *name = text_of(call->argument, member);
    if (!is_command_name(name)) {
        return refuse(call, "use letters, digits, - _ and ., starting with a letter or digit, at most 64",
                      "the command name %s is not a portable file name", name == NULL ? "(none)" : name);
    }
    return 1;
}

/**
 * @implNote Refused rather than read as an empty PATH: with no PATH every name looks free, so a host
 * asking for cc would shadow the compiler.
 */
static int validated_environment(command_call *call) {
    const cJSON *environment = cJSON_GetObjectItemCaseSensitive(call->argument, "environment");
    if (environment != NULL && !cJSON_IsObject(environment)) {
        return refuse(call, "send the user's environment as an object of names to strings", "%s",
                      "the call's environment is not an object");
    }
    if (environment_value(environment, "PATH") != NULL) {
        return 1;
    }
    return refuse(call, "send the user's environment with its PATH as a string", "%s",
                  "the call carries no PATH, so it cannot tell which names are taken");
}

static int validated_target(command_call *call) {
    const char *defect = target_defect(text_of(call->argument, "target"));
    return defect == NULL || refuse(call, "send the absolute path of the program the command runs", "%s", defect);
}

static int validated_arguments(command_call *call) {
    const char *defect = arguments_defect(cJSON_GetObjectItemCaseSensitive(call->argument, "arguments"));
    return defect == NULL ||
           refuse(call, "send the arguments the command always passes as strings, each without a line break", "%s",
                  defect);
}

static int answer_existing(command_call *call, cJSON *entry) {
    if (!is_active(entry)) {
        call->answers_name = 0;
        return 1;
    }
    snprintf(call->name, sizeof call->name, "%s", text_of(entry, "name"));
    call->answers_name = 1;
    return make_shim(call, call->name);
}

static int give_new_command(command_call *call, const char *requested, const char *owner) {
    if (!make_shim(call, call->name) || !load_registration(call) ||
        !append_entry(call, requested, owner) || !store_registration(call)) {
        return 0;
    }
    call->answers_name = 1;
    return 1;
}

static int request_step(command_call *call) {
    if (!validated_name(call, "name") || !validated_target(call) || !validated_arguments(call) ||
        !validated_environment(call) || !load_registration(call)) {
        return 0;
    }
    const char *requested = text_of(call->argument, "name");
    cJSON *existing = entry_for_requested(call->registration, requested);
    if (existing != NULL) {
        return answer_existing(call, existing);
    }
    return assign_name(call, requested) && give_new_command(call, requested, "host");
}

static int add_step(command_call *call) {
    if (!validated_name(call, "name") || !validated_target(call) || !validated_arguments(call) ||
        !validated_environment(call) || !load_registration(call)) {
        return 0;
    }
    const char *name = text_of(call->argument, "name");
    if (is_name_held(call, name)) {
        return refuse(call, "choose another name, or rename what holds it", "the command name %s is held", name);
    }
    snprintf(call->name, sizeof call->name, "%s", name);
    return give_new_command(call, NULL, "user");
}

static int refuse_unknown_command(command_call *call, const char *name) {
    return refuse(call, "name one of the host's active commands", "the host has no active command named %s", name);
}

static cJSON *copy_change(const char *path, const char *source) {
    cJSON *change = path == NULL || source == NULL ? NULL : change_of("file", "path", path);
    if (change != NULL && (cJSON_AddStringToObject(change, "source", source) == NULL ||
                           cJSON_AddTrueToObject(change, "executable") == NULL)) {
        cJSON_Delete(change);
        return NULL;
    }
    return change;
}

/** Copies one shim file of from to the name to, unchanged, since no shim file names itself. */
static int copy_shim_file(command_call *call, const char *from, const char *to, size_t index) {
    char *source = shim_file(call->shims, from, index);
    char *path = shim_file(call->shims, to, index);
    int copied = 0;
    if (source != NULL && runtime_probe_entry(source) == RUNTIME_ENTRY_MISSING) {
        refuse(call, "let the host ask for the command again, which writes its shim", "the shim of %s is missing",
               from);
    } else {
        copied = apply_change(call, copy_change(path, source));
    }
    free(source);
    free(path);
    return copied;
}

static int move_command(command_call *call, const char *from, const char *to) {
    int moved = 1;
    for (size_t index = 0; index < SHIM_FILE_COUNT && moved; index++) {
        moved = copy_shim_file(call, from, to, index);
    }
    moved = moved && load_registration(call);
    cJSON *entry = moved ? active_named(call->registration, from) : NULL;
    if (moved && entry == NULL) {
        return refuse_unknown_command(call, from);
    }
    return moved && set_text(entry, "name", to) && set_text(entry, "owner", "user") && remove_shim(call, from) &&
           store_registration(call);
}

static int rename_step(command_call *call) {
    if (!validated_name(call, "from") || !validated_name(call, "to") || !validated_environment(call) ||
        !load_registration(call)) {
        return 0;
    }
    const char *from = text_of(call->argument, "from");
    const char *to = text_of(call->argument, "to");
    if (active_named(call->registration, from) == NULL) {
        return refuse_unknown_command(call, from);
    }
    snprintf(call->name, sizeof call->name, "%s", to);
    call->answers_name = 1;
    if (strcmp(from, to) == 0) {
        return 1;
    }
    if (is_name_held(call, to)) {
        return refuse(call, "choose another name, or rename what holds it", "the command name %s is held", to);
    }
    return move_command(call, from, to);
}

static int delete_step(command_call *call) {
    if (!validated_name(call, "name") || !load_registration(call)) {
        return 0;
    }
    const char *name = text_of(call->argument, "name");
    cJSON *entry = active_named(call->registration, name);
    if (entry == NULL) {
        return refuse_unknown_command(call, name);
    }
    int required = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(call->registration, "requiresCommand"));
    if (required && active_count(call->registration) == 1) {
        return refuse(call, "rename it instead, or add another command first",
                      "the host %s requires a command and this is its last one", call->host);
    }
    call->answers_deleted = 1;
    return set_text(entry, "state", "deleted") && set_text(entry, "owner", "user") && remove_shim(call, name) &&
           store_registration(call);
}

static int32_t answer(const command_call *call, uint8_t **out, size_t *out_len) {
    cJSON *result = cJSON_CreateObject();
    int complete = result != NULL && (!call->answers_name || cJSON_AddStringToObject(result, "name", call->name) != NULL) &&
                   (!call->answers_deleted || cJSON_AddTrueToObject(result, "deleted") != NULL);
    int32_t status = SPISOR_STATUS_OUT_OF_MEMORY;
    if (complete) {
        status = spisor_succeed(result, out, out_len);
    } else {
        *out = NULL;
        *out_len = 0;
    }
    cJSON_Delete(result);
    return status;
}

static int32_t run_locked(command_call *call, command_step step, uint8_t **out, size_t *out_len) {
    spisor_lock *guard = runtime_hosts_lock(call->state);
    if (guard == NULL) {
        return spisor_fail("the registry lock could not be taken; nothing was changed",
                           "check that the hosts directory under the state place can be written", out, out_len);
    }
    int done = step(call);
    spisor_unlock(guard);
    return done ? answer(call, out, out_len) : spisor_fail(call->reason, call->fix, out, out_len);
}

static int32_t run(const char *state, const char *host, const cJSON *argument, command_step step, uint8_t **out,
                   size_t *out_len) {
    command_call call;
    memset(&call, 0, sizeof call);
    call.state = state;
    call.host = host;
    call.argument = argument;
    call.shims = joined(state, "bin");
    int32_t status = call.shims == NULL ? spisor_fail("memory ran out", "try again", out, out_len)
                                        : run_locked(&call, step, out, out_len);
    cJSON_Delete(call.registration);
    free(call.shims);
    return status;
}

static int32_t serve_request(const char *state, const char *host, const cJSON *argument, uint8_t **out,
                             size_t *out_len) {
    return run(state, host, argument, request_step, out, out_len);
}

static int32_t serve_rename(const char *state, const char *host, const cJSON *argument, uint8_t **out,
                            size_t *out_len) {
    return run(state, host, argument, rename_step, out, out_len);
}

static int32_t serve_delete(const char *state, const char *host, const cJSON *argument, uint8_t **out,
                            size_t *out_len) {
    return run(state, host, argument, delete_step, out, out_len);
}

static int32_t serve_add(const char *state, const char *host, const cJSON *argument, uint8_t **out,
                         size_t *out_len) {
    return run(state, host, argument, add_step, out, out_len);
}

RUNTIME_EXPORT int32_t runtime_commands_request(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_changes_serve(in, in_len, serve_request, out, out_len);
}

RUNTIME_EXPORT int32_t runtime_commands_rename(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_changes_serve(in, in_len, serve_rename, out, out_len);
}

RUNTIME_EXPORT int32_t runtime_commands_delete(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_changes_serve(in, in_len, serve_delete, out, out_len);
}

RUNTIME_EXPORT int32_t runtime_commands_add(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_changes_serve(in, in_len, serve_add, out, out_len);
}
