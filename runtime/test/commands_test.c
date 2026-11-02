#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#include <wchar.h>
#include <windows.h>
#define CURRENT_PROCESS_ID _getpid()
#define PATH_LIST_SEPARATOR ";"
#define SHIM_FILE_COUNT 2
#define PROGRAM_SUFFIX ".exe"
#else
#include <sys/wait.h>
#include <unistd.h>
#define CURRENT_PROCESS_ID getpid()
#define PATH_LIST_SEPARATOR ":"
#define SHIM_FILE_COUNT 1
#define PROGRAM_SUFFIX ""
#endif

#include "changes.h"
#include "check.h"
#include "commands.h"
#include "directory_tree.h"
#include "registry.h"
#include "scratch_directory.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"

#ifdef _WIN32
#include "path_entry_windows_hooks.h"
#endif

typedef int32_t (*runtime_export)(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

typedef struct scratch {
    char directory[1024];
    char home[1100];
    char state[1100];
    char shims[1200];
    char programs[1100];
    char targets[1100];
} scratch;

typedef struct answer {
    int32_t status;
    cJSON *tree;
} answer;

static void fresh_scratch(scratch *place, const char *label) {
    static int sequence = 0;
    snprintf(place->directory, sizeof place->directory, RUNTIME_SCRATCH_PATH "/commands-%d-%d-%s",
             (int)CURRENT_PROCESS_ID, sequence++, label);
    snprintf(place->home, sizeof place->home, "%s/home", place->directory);
    snprintf(place->state, sizeof place->state, "%s/state", place->directory);
    snprintf(place->shims, sizeof place->shims, "%s/bin", place->state);
    snprintf(place->programs, sizeof place->programs, "%s/programs", place->directory);
    snprintf(place->targets, sizeof place->targets, "%s/targets", place->directory);
    start_empty_directory(place->directory);
    CHECK(spisor_make_directories(place->home) == 1);
    CHECK(spisor_make_directories(place->state) == 1);
    CHECK(spisor_make_directories(place->programs) == 1);
    CHECK(spisor_make_directories(place->targets) == 1);
    char profile[1200];
    snprintf(profile, sizeof profile, "%s/.profile", place->home);
    CHECK(spisor_replace_file(profile, (const uint8_t *)"# profile\n", 10) == 1);
}

static answer call(runtime_export function, cJSON *request) {
    char *text = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    uint8_t *out = NULL;
    size_t out_len = 0;
    answer result;
    result.status = function((const uint8_t *)text, strlen(text), &out, &out_len);
    result.tree = out == NULL ? NULL : spisor_parse_json(out, out_len);
    free(out);
    free(text);
    return result;
}

static void report(const char *what, const answer *result) {
    char *text = result->tree == NULL ? NULL : cJSON_PrintUnformatted(result->tree);
    fprintf(stderr, "%s: status %d %s\n", what, (int)result->status, text == NULL ? "" : text);
    free(text);
}

static cJSON *host_request(const scratch *place, const char *host) {
    cJSON *request = cJSON_CreateObject();
    cJSON *places = cJSON_AddObjectToObject(request, "places");
    cJSON_AddStringToObject(places, "state", place->state);
    cJSON_AddStringToObject(request, "host", host);
    return request;
}

static void register_host(const scratch *place, const char *id, int requires_command) {
    cJSON *request = host_request(place, id);
    cJSON_DeleteItemFromObject(request, "host");
    cJSON *registration = cJSON_AddObjectToObject(request, "registration");
    cJSON_AddStringToObject(registration, "id", id);
    cJSON_AddStringToObject(registration, "kind", "host");
    cJSON_AddStringToObject(registration, "version", "1.0.0");
    cJSON *provenance = cJSON_AddObjectToObject(registration, "provenance");
    cJSON_AddStringToObject(provenance, "method", "npm");
    cJSON_AddStringToObject(provenance, "version", "1.0.0");
    cJSON_AddStringToObject(provenance, "location", place->directory);
    cJSON_AddBoolToObject(registration, "requiresCommand", requires_command);
    answer result = call(runtime_registry_register, request);
    CHECK(result.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(result.tree);
}

static void add_environment(cJSON *request, const char *search) {
    cJSON *environment = cJSON_AddObjectToObject(request, "environment");
    cJSON_AddStringToObject(environment, "PATH", search);
#ifdef _WIN32
    cJSON_AddStringToObject(environment, "PATHEXT", ".COM;.EXE");
#endif
}

static void default_search(const scratch *place, char *search, size_t size) {
    snprintf(search, size, "%s" PATH_LIST_SEPARATOR "%s", place->programs, place->shims);
}

static void target_path(const scratch *place, const char *program, char *path, size_t size) {
    snprintf(path, size, "%s/%s" PROGRAM_SUFFIX, place->targets, program);
}

static cJSON *targeted_request(const scratch *place, const char *host, const char *search, const char *name,
                               const char *target) {
    cJSON *request = host_request(place, host);
    add_environment(request, search);
    cJSON_AddStringToObject(request, "home", place->home);
    cJSON_AddStringToObject(request, "name", name);
    cJSON_AddStringToObject(request, "target", target);
    return request;
}

static cJSON *naming_request(const scratch *place, const char *host, const char *search, const char *name,
                             const char *program) {
    char target[1300];
    target_path(place, program, target, sizeof target);
    return targeted_request(place, host, search, name, target);
}

/** The name answered, "" when none, or NULL when the call fails; freed with free. */
static char *answered_name(answer result, const char *what) {
    char *assigned = NULL;
    if (result.status == SPISOR_STATUS_SUCCESS) {
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result.tree, "name"));
        assigned = text == NULL ? calloc(1, 1) : malloc(strlen(text) + 1);
        if (assigned != NULL && text != NULL) {
            memcpy(assigned, text, strlen(text) + 1);
        }
    } else {
        report(what, &result);
    }
    cJSON_Delete(result.tree);
    return assigned;
}

static char *requested_on(const scratch *place, const char *host, const char *search, const char *name,
                          const char *program) {
    return answered_name(call(runtime_commands_request, naming_request(place, host, search, name, program)),
                         "request");
}

static int requests(const scratch *place, const char *host, const char *name, const char *expected) {
    char search[2500];
    default_search(place, search, sizeof search);
    char *assigned = requested_on(place, host, search, name, "tool");
    int same = check_text(assigned, expected, name);
    free(assigned);
    return same;
}

static answer rename_command(const scratch *place, const char *host, const char *from, const char *to) {
    char search[2500];
    default_search(place, search, sizeof search);
    cJSON *request = host_request(place, host);
    add_environment(request, search);
    cJSON_AddStringToObject(request, "from", from);
    cJSON_AddStringToObject(request, "to", to);
    return call(runtime_commands_rename, request);
}

static answer delete_command(const scratch *place, const char *host, const char *name) {
    cJSON *request = host_request(place, host);
    cJSON_AddStringToObject(request, "name", name);
    return call(runtime_commands_delete, request);
}

static answer add_program(const scratch *place, const char *host, const char *name, const char *program) {
    char search[2500];
    default_search(place, search, sizeof search);
    return call(runtime_commands_add, naming_request(place, host, search, name, program));
}

static answer add_command(const scratch *place, const char *host, const char *name) {
    return add_program(place, host, name, "tool");
}

static int succeeds(answer result, const char *what) {
    int succeeded = result.status == SPISOR_STATUS_SUCCESS;
    if (!succeeded) {
        report(what, &result);
    }
    cJSON_Delete(result.tree);
    return succeeded;
}

static int is_refused(answer result, const char *what) {
    int refused = result.status == SPISOR_STATUS_FAILURE;
    if (!refused) {
        report(what, &result);
    }
    cJSON_Delete(result.tree);
    return refused;
}

/** The file a shim is run by: <name>.exe on Windows, beside its <name>.shim, and <name> on POSIX. */
static void shim_file(const scratch *place, const char *name, char *path, size_t size) {
    snprintf(path, size, "%s/%s" PROGRAM_SUFFIX, place->shims, name);
}

static void shim_part(const scratch *place, const char *name, size_t index, char *path, size_t size) {
    if (index == 0) {
        shim_file(place, name, path, size);
    } else {
        snprintf(path, size, "%s/%s.shim", place->shims, name);
    }
}

static int shim_parts_present(const scratch *place, const char *name) {
    int present = 0;
    for (size_t index = 0; index < SHIM_FILE_COUNT; index++) {
        char path[1400];
        shim_part(place, name, index, path, sizeof path);
        present += runtime_probe_entry(path) == RUNTIME_ENTRY_OTHER;
    }
    return present;
}

static int has_shim(const scratch *place, const char *name) {
    return shim_parts_present(place, name) == SHIM_FILE_COUNT;
}

static int lacks_shim(const scratch *place, const char *name) {
    return shim_parts_present(place, name) == 0;
}

/** Every byte of every file of the shim of name, freed with free. */
static char *shim_bytes(const scratch *place, const char *name) {
    size_t used = 0;
    char *all = calloc(1, 1);
    for (size_t index = 0; index < SHIM_FILE_COUNT && all != NULL; index++) {
        char path[1400];
        shim_part(place, name, index, path, sizeof path);
        size_t length = 0;
        uint8_t *bytes = spisor_read_file(path, &length);
        char *grown = bytes == NULL ? NULL : realloc(all, used + length + 1);
        if (grown != NULL) {
            memcpy(grown + used, bytes, length);
            used += length;
            grown[used] = '\0';
            all = grown;
        }
        free(bytes);
    }
    return all;
}

static cJSON *loaded(const scratch *place, const char *host) {
    cJSON *registration = NULL;
    CHECK(runtime_registry_load(place->state, host, &registration) == RUNTIME_REGISTRY_OK);
    return registration;
}

/** Whether host's entry for requested is exactly name, owner and state. */
static int entry_is(const scratch *place, const char *host, const char *requested, const char *name, const char *owner,
                    const char *state) {
    cJSON *registration = loaded(place, host);
    int matches = 0;
    int count = 0;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(registration, "commands")) {
        const char *entry_requested = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "requested"));
        if (entry_requested == NULL || strcmp(entry_requested, requested) != 0) {
            continue;
        }
        count++;
        matches = strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "name")), name) == 0 &&
                  strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "owner")), owner) == 0 &&
                  strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "state")), state) == 0;
    }
    if (count != 1 || !matches) {
        char *text = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(registration, "commands"));
        fprintf(stderr, "%s: expected one entry for %s as %s/%s/%s, got %s\n", host, requested, name, owner, state,
                text == NULL ? "" : text);
        free(text);
        check_failures++;
    }
    cJSON_Delete(registration);
    return count == 1 && matches;
}

/** Whether host has exactly one entry the user added under name, which no host requested, in state. */
static int user_entry_is(const scratch *place, const char *host, const char *name, const char *state) {
    cJSON *registration = loaded(place, host);
    int count = 0;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(registration, "commands")) {
        const char *entry_name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "name"));
        const char *entry_state = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "state"));
        const char *owner = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "owner"));
        count += cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(entry, "requested")) && entry_name != NULL &&
                 strcmp(entry_name, name) == 0 && owner != NULL && strcmp(owner, "user") == 0 &&
                 entry_state != NULL && strcmp(entry_state, state) == 0;
    }
    if (count != 1) {
        char *text = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(registration, "commands"));
        fprintf(stderr, "%s: expected one user-added entry %s %s, got %s\n", host, name, state,
                text == NULL ? "" : text);
        free(text);
        check_failures++;
    }
    cJSON_Delete(registration);
    return count == 1;
}

static int reverse_host(const scratch *place, const char *host) {
    answer result = call(runtime_changes_reverse_json, host_request(place, host));
    int clean = result.status == SPISOR_STATUS_SUCCESS &&
                cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(result.tree, "drifted")) == 0;
    if (!clean) {
        report("reverse", &result);
    }
    cJSON_Delete(result.tree);
    return clean;
}

static void write_program(const char *path, const char *content, int executable) {
    CHECK(runtime_create_new_file(path, (const uint8_t *)content, strlen(content), executable) == RUNTIME_ENTRY_DONE);
}

static void place_program_on_path(const scratch *place, const char *name, int executable) {
    char path[1300];
#ifdef _WIN32
    (void)executable;
    snprintf(path, sizeof path, "%s/%s.exe", place->programs, name);
#else
    snprintf(path, sizeof path, "%s/%s", place->programs, name);
#endif
    write_program(path, "program\n", executable);
}

static void first_request_gets_its_name_and_later_hosts_fall_back(void) {
    scratch place;
    fresh_scratch(&place, "fallback");
    register_host(&place, "alpha", 0);
    register_host(&place, "beta", 0);
    register_host(&place, "gamma", 0);
    CHECK(requests(&place, "alpha", "oc", "oc"));
    CHECK(requests(&place, "beta", "oc", "oc2"));
    CHECK(requests(&place, "gamma", "oc", "oc3"));
    CHECK(requests(&place, "alpha", "oc", "oc"));
    CHECK(requests(&place, "beta", "oc", "oc2"));
    CHECK(entry_is(&place, "alpha", "oc", "oc", "host", "active"));
    CHECK(entry_is(&place, "beta", "oc", "oc2", "host", "active"));
    CHECK(has_shim(&place, "oc") && has_shim(&place, "oc2") && has_shim(&place, "oc3"));
    CHECK(reverse_host(&place, "alpha"));
    CHECK(reverse_host(&place, "beta"));
    CHECK(reverse_host(&place, "gamma"));
}

static void a_program_on_path_is_never_shadowed(void) {
    scratch place;
    fresh_scratch(&place, "on-path");
    register_host(&place, "alpha", 1);
    place_program_on_path(&place, "cc", 1);
    CHECK(requests(&place, "alpha", "cc", "cc2"));
    CHECK(lacks_shim(&place, "cc") && has_shim(&place, "cc2"));
    CHECK(entry_is(&place, "alpha", "cc", "cc2", "host", "active"));
#ifndef _WIN32
    place_program_on_path(&place, "ld", 0);
    CHECK(requests(&place, "alpha", "ld", "ld"));
    if (access("/usr/bin/cc", X_OK) == 0) {
        char *assigned = requested_on(&place, "alpha", "/usr/bin", "cc", "tool");
        CHECK(check_text(assigned, "cc2", "the system cc"));
        free(assigned);
    }
#else
    cJSON *environment = cJSON_CreateObject();
    char search[1400];
    snprintf(search, sizeof search, "\"%s\"", place.programs);
    cJSON_AddStringToObject(environment, "Path", search);
    CHECK(runtime_commands_held_on_path(environment, place.shims, "cc"));
    CHECK(runtime_commands_held_on_path(environment, place.shims, "cc.exe"));
    CHECK(!runtime_commands_held_on_path(environment, place.shims, "ld"));
    cJSON_Delete(environment);
#endif
    CHECK(reverse_host(&place, "alpha"));
}

static void a_user_rename_survives_the_next_request(void) {
    scratch place;
    fresh_scratch(&place, "rename");
    register_host(&place, "alpha", 1);
    CHECK(requests(&place, "alpha", "cc", "cc"));
    CHECK(succeeds(rename_command(&place, "alpha", "cc", "claude"), "rename"));
    CHECK(lacks_shim(&place, "cc") && has_shim(&place, "claude"));
    CHECK(requests(&place, "alpha", "cc", "claude"));
    CHECK(entry_is(&place, "alpha", "cc", "claude", "user", "active"));
    CHECK(lacks_shim(&place, "cc") && has_shim(&place, "claude"));
    CHECK(reverse_host(&place, "alpha"));
    CHECK(lacks_shim(&place, "claude"));
}

static void a_user_delete_is_not_undone_by_the_next_request(void) {
    scratch place;
    fresh_scratch(&place, "delete");
    register_host(&place, "alpha", 0);
    CHECK(requests(&place, "alpha", "oc", "oc"));
    CHECK(succeeds(delete_command(&place, "alpha", "oc"), "delete"));
    CHECK(lacks_shim(&place, "oc"));
    CHECK(requests(&place, "alpha", "oc", ""));
    CHECK(lacks_shim(&place, "oc"));
    CHECK(entry_is(&place, "alpha", "oc", "oc", "user", "deleted"));
    CHECK(reverse_host(&place, "alpha"));
}

static void a_command_the_user_added_is_never_taken_by_a_host(void) {
    scratch place;
    fresh_scratch(&place, "user-added");
    register_host(&place, "alpha", 0);
    CHECK(requests(&place, "alpha", "cc", "cc"));
    CHECK(succeeds(delete_command(&place, "alpha", "cc"), "delete"));
    CHECK(succeeds(add_program(&place, "alpha", "cc", "mine"), "add the user's own cc"));
    char *before = shim_bytes(&place, "cc");
    CHECK(requests(&place, "alpha", "cc", ""));
    char *after = shim_bytes(&place, "cc");
    CHECK(before != NULL && before[0] != '\0' && check_text(after, before, "the user's shim after the request"));
    CHECK(entry_is(&place, "alpha", "cc", "cc", "user", "deleted"));
    CHECK(user_entry_is(&place, "alpha", "cc", "active"));
    free(before);
    free(after);
    CHECK(reverse_host(&place, "alpha"));
}

static void a_required_host_keeps_its_last_command(void) {
    scratch place;
    fresh_scratch(&place, "required");
    register_host(&place, "alpha", 1);
    CHECK(requests(&place, "alpha", "cc", "cc"));
    CHECK(is_refused(delete_command(&place, "alpha", "cc"), "delete the last command"));
    CHECK(has_shim(&place, "cc"));
    CHECK(succeeds(rename_command(&place, "alpha", "cc", "cl"), "rename the last command"));
    CHECK(is_refused(delete_command(&place, "alpha", "cl"), "delete the renamed last command"));
    CHECK(succeeds(add_command(&place, "alpha", "extra"), "add"));
    CHECK(user_entry_is(&place, "alpha", "extra", "active"));
    CHECK(succeeds(delete_command(&place, "alpha", "cl"), "delete one of two"));
    CHECK(is_refused(delete_command(&place, "alpha", "extra"), "delete the last added command"));
    CHECK(has_shim(&place, "extra") && lacks_shim(&place, "cl"));
    CHECK(reverse_host(&place, "alpha"));
}

static void rename_and_add_refuse_a_held_name(void) {
    scratch place;
    fresh_scratch(&place, "held");
    register_host(&place, "alpha", 0);
    register_host(&place, "beta", 0);
    place_program_on_path(&place, "cc", 1);
    CHECK(requests(&place, "alpha", "oc", "oc"));
    CHECK(requests(&place, "beta", "bc", "bc"));
    CHECK(is_refused(rename_command(&place, "alpha", "oc", "bc"), "rename onto another host's command"));
    CHECK(is_refused(rename_command(&place, "alpha", "oc", "cc"), "rename onto a program on PATH"));
    CHECK(is_refused(add_command(&place, "alpha", "bc"), "add another host's command"));
    CHECK(is_refused(add_command(&place, "alpha", "cc"), "add a program on PATH"));
    CHECK(is_refused(rename_command(&place, "alpha", "nothing", "free"), "rename a command the host lacks"));
    CHECK(is_refused(add_command(&place, "alpha", "../escape"), "add a name that is not a file name"));
    CHECK(entry_is(&place, "alpha", "oc", "oc", "host", "active"));
    CHECK(has_shim(&place, "oc") && has_shim(&place, "bc") && lacks_shim(&place, "cc"));
    CHECK(reverse_host(&place, "alpha"));
    CHECK(reverse_host(&place, "beta"));
}

static cJSON *without_path(cJSON *request, const char *shape) {
    cJSON_DeleteItemFromObjectCaseSensitive(request, "environment");
    if (strcmp(shape, "no environment") == 0) {
        return request;
    }
    cJSON *environment = cJSON_AddObjectToObject(request, "environment");
    if (strcmp(shape, "a PATH that is not text") == 0) {
        cJSON_AddNumberToObject(environment, "PATH", 1);
    }
    cJSON_AddStringToObject(environment, "SHELL", "/bin/sh");
    return request;
}

static void a_call_without_a_path_is_refused_and_changes_nothing(void) {
    static const char *const shapes[] = {"no environment", "no PATH", "a PATH that is not text"};
    scratch place;
    fresh_scratch(&place, "no-path");
    register_host(&place, "alpha", 0);
    CHECK(requests(&place, "alpha", "oc", "oc"));
    for (size_t index = 0; index < sizeof shapes / sizeof shapes[0]; index++) {
        char search[2500];
        default_search(&place, search, sizeof search);
        CHECK(is_refused(call(runtime_commands_request,
                              without_path(naming_request(&place, "alpha", search, "cc", "tool"), shapes[index])),
                         shapes[index]));
        CHECK(is_refused(call(runtime_commands_add,
                              without_path(naming_request(&place, "alpha", search, "ad", "tool"), shapes[index])),
                         shapes[index]));
        cJSON *rename = host_request(&place, "alpha");
        cJSON_AddStringToObject(rename, "from", "oc");
        cJSON_AddStringToObject(rename, "to", "rn");
        CHECK(is_refused(call(runtime_commands_rename, without_path(rename, shapes[index])), shapes[index]));
    }
    CHECK(lacks_shim(&place, "cc") && lacks_shim(&place, "ad") && lacks_shim(&place, "rn"));
    CHECK(has_shim(&place, "oc"));
    CHECK(entry_is(&place, "alpha", "oc", "oc", "host", "active"));
    CHECK(reverse_host(&place, "alpha"));
}

#ifdef _WIN32
static const char *const odd_directory = "J\xc3\xbcrgen's tools";

static size_t append_wide(wchar_t *line, size_t used, size_t size, wchar_t character) {
    if (used + 1 < size) {
        line[used++] = character;
        line[used] = L'\0';
    }
    return used;
}

/** Appends argument quoted as CommandLineToArgvW reads it back: backslashes doubled before a quote. */
static size_t append_quoted(wchar_t *line, size_t used, size_t size, const char *argument) {
    wchar_t wide[2048];
    MultiByteToWideChar(CP_UTF8, 0, argument, -1, wide, 2048);
    used = append_wide(line, used, size, L'"');
    size_t backslashes = 0;
    for (const wchar_t *cursor = wide; *cursor != L'\0'; cursor++) {
        if (*cursor == L'\\') {
            backslashes++;
            continue;
        }
        size_t repeat = *cursor == L'"' ? 2 * backslashes + 1 : backslashes;
        for (size_t index = 0; index < repeat; index++) {
            used = append_wide(line, used, size, L'\\');
        }
        backslashes = 0;
        used = append_wide(line, used, size, *cursor);
    }
    for (size_t index = 0; index < 2 * backslashes; index++) {
        used = append_wide(line, used, size, L'\\');
    }
    return append_wide(line, used, size, L'"');
}

/** Runs program with arguments, as a terminal would, and answers its exit code, or -1 when it did not start. */
static int run_program(const char *program, const char *const *arguments, size_t count) {
    wchar_t line[8192];
    line[0] = L'\0';
    size_t used = append_quoted(line, 0, 8192, program);
    for (size_t index = 0; index < count; index++) {
        used = append_wide(line, used, 8192, L' ');
        used = append_quoted(line, used, 8192, arguments[index]);
    }
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    ZeroMemory(&startup, sizeof startup);
    startup.cb = sizeof startup;
    if (!CreateProcessW(NULL, line, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
        return -1;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return (int)code;
}
#else
static const char *const odd_directory = "it's $HOME dir";

static int run_program(const char *program, const char *const *arguments, size_t count) {
    char *values[16];
    values[0] = (char *)program;
    for (size_t index = 0; index < count && index < 14; index++) {
        values[index + 1] = (char *)arguments[index];
    }
    values[count + 1] = NULL;
    pid_t child = fork();
    if (child == 0) {
        execv(program, values);
        _exit(126);
    }
    int status = 0;
    if (child < 0 || waitpid(child, &status, 0) != child) {
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
#endif

/** Copies the probe program to directory under the scratch targets and fills target with its path. */
static void place_probe(const scratch *place, const char *directory, char *target, size_t size) {
    char folder[1300];
    snprintf(folder, sizeof folder, "%s/%s", place->targets, directory);
    CHECK(spisor_make_directories(folder) == 1);
    snprintf(target, size, "%s/probe" PROGRAM_SUFFIX, folder);
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(RUNTIME_PROBE_PATH, &length);
    CHECK(bytes != NULL);
    CHECK(bytes != NULL && runtime_create_new_file(target, bytes, length, 1) == RUNTIME_ENTRY_DONE);
    free(bytes);
}

static char *request_target(const scratch *place, const char *host, const char *name, const char *target) {
    char search[2500];
    default_search(place, search, sizeof search);
    return answered_name(call(runtime_commands_request, targeted_request(place, host, search, name, target)),
                         "request");
}

static const char *const awkward_arguments = "one\ntwo words\nsay \"hi\"\nit's $x\n";

/** Runs the shim of name with awkward arguments, asking the probe to exit with exit_code. */
static int shim_passes_through(const scratch *place, const char *name, const char *exit_code) {
    char shim[1400];
    char output[1400];
    shim_file(place, name, shim, sizeof shim);
    snprintf(output, sizeof output, "%s/%s-%s.out", place->targets, name, exit_code);
    const char *const arguments[] = {output, exit_code, "one", "two words", "say \"hi\"", "it's $x"};
    int code = run_program(shim, arguments, 6);
    size_t length = 0;
    char *received = (char *)spisor_read_file(output, &length);
    int passed = code == atoi(exit_code) && check_text(received, awkward_arguments, "arguments the target received");
    if (code != atoi(exit_code)) {
        fprintf(stderr, "the shim %s exited %d, expected %s\n", name, code, exit_code);
    }
    free(received);
    return passed;
}

static void the_shim_runs_its_target_with_the_arguments_and_exit_code(void) {
    scratch place;
    fresh_scratch(&place, "runs");
    register_host(&place, "alpha", 1);
    char target[1400];
    char moved[1400];
    place_probe(&place, odd_directory, target, sizeof target);
    place_probe(&place, "moved elsewhere", moved, sizeof moved);
    char *assigned = request_target(&place, "alpha", "cc", target);
    CHECK(check_text(assigned, "cc", "the first request"));
    free(assigned);
    CHECK(shim_passes_through(&place, "cc", "7"));
    assigned = request_target(&place, "alpha", "cc", moved);
    CHECK(check_text(assigned, "cc", "the request after the target moved"));
    free(assigned);
    CHECK(shim_passes_through(&place, "cc", "0"));
    CHECK(succeeds(rename_command(&place, "alpha", "cc", "renamed"), "rename"));
    CHECK(shim_passes_through(&place, "renamed", "3"));
    CHECK(reverse_host(&place, "alpha"));
}

static int path_entry_count(const scratch *place, const char *host) {
    cJSON *registration = loaded(place, host);
    int count = 0;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(registration, "changes")) {
        const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "kind"));
        count += kind != NULL && strcmp(kind, "path-entry") == 0;
    }
    cJSON_Delete(registration);
    return count;
}

#ifdef _WIN32
#define ENVIRONMENT_KEY L"Environment"
#define READ_FLAGS (RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND)

typedef struct registry_text {
    wchar_t text[8192];
    DWORD type;
    int present;
} registry_text;

static void read_value(const wchar_t *name, registry_text *value) {
    DWORD size = sizeof value->text;
    value->type = 0;
    value->text[0] = L'\0';
    LSTATUS status =
        RegGetValueW(HKEY_CURRENT_USER, ENVIRONMENT_KEY, name, READ_FLAGS, &value->type, value->text, &size);
    value->present = status == ERROR_SUCCESS;
}

static void delete_test_value(void) {
    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, ENVIRONMENT_KEY, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, RUNTIME_PATH_ENTRY_TEST_VALUE);
        RegCloseKey(key);
    }
}

static int shims_on_path(const scratch *place) {
    wchar_t wide[1400];
    char backslashed[1400];
    snprintf(backslashed, sizeof backslashed, "%s", place->shims);
    for (char *cursor = backslashed; *cursor != '\0'; cursor++) {
        *cursor = *cursor == '/' ? '\\' : *cursor;
    }
    MultiByteToWideChar(CP_UTF8, 0, backslashed, -1, wide, 1400);
    registry_text value;
    read_value(RUNTIME_PATH_ENTRY_TEST_VALUE, &value);
    return value.present && wcsstr(value.text, wide) != NULL;
}
#else
static int shims_on_path(const scratch *place) {
    char profile[1200];
    char line[1400];
    snprintf(profile, sizeof profile, "%s/.profile", place->home);
    snprintf(line, sizeof line, "export PATH=\"%s:$PATH\"", place->shims);
    size_t length = 0;
    char *text = (char *)spisor_read_file(profile, &length);
    int found = text != NULL && strstr(text, line) != NULL;
    free(text);
    return found;
}
#endif

static void the_first_command_puts_the_shims_on_path_until_the_last_host_goes(void) {
    scratch place;
    fresh_scratch(&place, "path");
    register_host(&place, "alpha", 0);
    register_host(&place, "beta", 0);
    CHECK(!shims_on_path(&place));
    CHECK(requests(&place, "alpha", "oc", "oc"));
    CHECK(shims_on_path(&place));
    CHECK(path_entry_count(&place, "alpha") == 1);
    CHECK(requests(&place, "alpha", "od", "od"));
    CHECK(requests(&place, "alpha", "oc", "oc"));
    CHECK(path_entry_count(&place, "alpha") == 1);
    CHECK(requests(&place, "beta", "bc", "bc"));
    CHECK(reverse_host(&place, "alpha"));
    CHECK(lacks_shim(&place, "oc") && lacks_shim(&place, "od") && has_shim(&place, "bc"));
    CHECK(shims_on_path(&place));
    CHECK(reverse_host(&place, "beta"));
    CHECK(!shims_on_path(&place));
    CHECK(runtime_probe_entry(place.shims) == RUNTIME_ENTRY_MISSING);
}

int main(void) {
#ifdef _WIN32
    registry_text real_before;
    read_value(L"Path", &real_before);
    delete_test_value();
#endif
    first_request_gets_its_name_and_later_hosts_fall_back();
    a_program_on_path_is_never_shadowed();
    a_user_rename_survives_the_next_request();
    a_user_delete_is_not_undone_by_the_next_request();
    a_command_the_user_added_is_never_taken_by_a_host();
    a_required_host_keeps_its_last_command();
    rename_and_add_refuse_a_held_name();
    a_call_without_a_path_is_refused_and_changes_nothing();
    the_shim_runs_its_target_with_the_arguments_and_exit_code();
    the_first_command_puts_the_shims_on_path_until_the_last_host_goes();
#ifdef _WIN32
    delete_test_value();
    registry_text real_after;
    read_value(L"Path", &real_after);
    CHECK(real_before.present == real_after.present && real_before.type == real_after.type &&
          wcscmp(real_before.text, real_after.text) == 0);
#endif
    return check_summary("commands_test");
}
