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
#define PROGRAM_SUFFIX ".exe"
#else
#include <unistd.h>
#define CURRENT_PROCESS_ID getpid()
#define PATH_LIST_SEPARATOR ":"
#define PROGRAM_SUFFIX ""
#endif

#include "changes.h"
#include "check.h"
#include "commands.h"
#include "directory_tree.h"
#include "host_provenance.h"
#include "registry.h"
#include "scratch_directory.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"
#include "tree_snapshot.h"

#ifdef _WIN32
#include "path_entry_windows_hooks.h"
#endif

typedef int32_t (*runtime_export)(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

typedef struct scratch {
    char directory[1024];
    char home[1100];
    char state[1100];
    char cache[1100];
    char shims[1200];
    char programs[1100];
    char portable[1100];
    char secrets[1100];
    char config[1200];
    char target[1200];
    char search[2500];
} scratch;

typedef struct answer {
    int32_t status;
    cJSON *tree;
} answer;

static const char *const application_config = "{\n  \"theme\": \"dark\"\n}\n";
static const char *const portable_bytes = "{\"portable\":true}\n";
static const char *const secrets_bytes = "{\"alpha\":{\"token\":\"kept\"}}\n";

static void write_text(const char *path, const char *text) {
    CHECK(spisor_replace_file(path, (const uint8_t *)text, strlen(text)) == 1);
}

static void fresh_scratch(scratch *place) {
    snprintf(place->directory, sizeof place->directory, RUNTIME_SCRATCH_PATH "/lifecycle-%d",
             (int)CURRENT_PROCESS_ID);
    snprintf(place->home, sizeof place->home, "%s/home", place->directory);
    snprintf(place->state, sizeof place->state, "%s/state", place->directory);
    snprintf(place->cache, sizeof place->cache, "%s/cache", place->directory);
    snprintf(place->shims, sizeof place->shims, "%s/bin", place->state);
    snprintf(place->programs, sizeof place->programs, "%s/programs", place->directory);
    snprintf(place->portable, sizeof place->portable, "%s/portable", place->directory);
    snprintf(place->secrets, sizeof place->secrets, "%s/secrets.json", place->directory);
    snprintf(place->config, sizeof place->config, "%s/.application/config.json", place->home);
    snprintf(place->target, sizeof place->target, "%s/tool/run" PROGRAM_SUFFIX, place->directory);
    snprintf(place->search, sizeof place->search, "%s" PATH_LIST_SEPARATOR "%s", place->programs, place->shims);
    start_empty_directory(place->directory);
    char application[1200];
    snprintf(application, sizeof application, "%s/.application", place->home);
    CHECK(spisor_make_directories(application) == 1);
    CHECK(spisor_make_directories(place->state) == 1);
    CHECK(spisor_make_directories(place->cache) == 1);
    CHECK(spisor_make_directories(place->programs) == 1);
    CHECK(spisor_make_directories(place->portable) == 1);
}

static void seed_scratch(const scratch *place) {
    char path[1300];
    snprintf(path, sizeof path, "%s/.profile", place->home);
    write_text(path, "# profile\nexport EDITOR=vi\n");
    write_text(place->config, application_config);
    snprintf(path, sizeof path, "%s/config.json", place->portable);
    write_text(path, portable_bytes);
    write_text(place->secrets, secrets_bytes);
    snprintf(path, sizeof path, "%s/cc" PROGRAM_SUFFIX, place->programs);
    CHECK(runtime_create_new_file(path, (const uint8_t *)"program\n", 8, 1) == RUNTIME_ENTRY_DONE);
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

static int succeeds(answer result, const char *what) {
    int succeeded = result.status == SPISOR_STATUS_SUCCESS;
    if (!succeeded) {
        char *text = result.tree == NULL ? NULL : cJSON_PrintUnformatted(result.tree);
        fprintf(stderr, "%s: status %d %s\n", what, (int)result.status, text == NULL ? "" : text);
        free(text);
    }
    cJSON_Delete(result.tree);
    return succeeded;
}

static cJSON *places_request(const scratch *place) {
    cJSON *request = cJSON_CreateObject();
    cJSON *places = cJSON_AddObjectToObject(request, "places");
    cJSON_AddStringToObject(places, "state", place->state);
    cJSON_AddStringToObject(places, "cache", place->cache);
    return request;
}

static cJSON *host_request(const scratch *place, const char *host) {
    cJSON *request = places_request(place);
    cJSON_AddStringToObject(request, "host", host);
    return request;
}

static cJSON *environment_request(const scratch *place, const char *host) {
    cJSON *request = host_request(place, host);
    cJSON *environment = cJSON_AddObjectToObject(request, "environment");
    cJSON_AddStringToObject(environment, "PATH", place->search);
    cJSON_AddStringToObject(environment, "SHELL", "/bin/sh");
#ifdef _WIN32
    cJSON_AddStringToObject(environment, "PATHEXT", ".COM;.EXE");
#endif
    return request;
}

static int registers(const scratch *place, const char *id) {
    cJSON *request = places_request(place);
    cJSON *registration = cJSON_AddObjectToObject(request, "registration");
    cJSON_AddStringToObject(registration, "id", id);
    cJSON_AddStringToObject(registration, "kind", "host");
    cJSON_AddStringToObject(registration, "version", "1.0.0");
    cJSON_AddStringToObject(registration, "home", place->home);
    add_host_provenance(registration, "npm", "1.0.0", place->directory);
    cJSON_AddBoolToObject(registration, "requiresCommand", 0);
    return succeeds(call(runtime_registry_register, request), id);
}

/** Whether host's request for name is answered with expected. */
static int requests(const scratch *place, const char *host, const char *name, const char *expected) {
    cJSON *request = environment_request(place, host);
    cJSON_AddStringToObject(request, "home", place->home);
    cJSON_AddStringToObject(request, "name", name);
    cJSON_AddStringToObject(request, "target", place->target);
    answer result = call(runtime_commands_request, request);
    const char *given = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result.tree, "name"));
    int matches = result.status == SPISOR_STATUS_SUCCESS && check_text(given, expected, name);
    cJSON_Delete(result.tree);
    return matches;
}

static int renames(const scratch *place, const char *host, const char *from, const char *to) {
    cJSON *request = environment_request(place, host);
    cJSON_AddStringToObject(request, "from", from);
    cJSON_AddStringToObject(request, "to", to);
    return succeeds(call(runtime_commands_rename, request), "rename");
}

static int deletes(const scratch *place, const char *host, const char *name) {
    cJSON *request = host_request(place, host);
    cJSON_AddStringToObject(request, "name", name);
    return succeeds(call(runtime_commands_delete, request), "delete");
}

static int sets_application_key(const scratch *place, const char *host) {
    cJSON *request = host_request(place, host);
    cJSON *change = cJSON_AddObjectToObject(request, "change");
    cJSON_AddStringToObject(change, "kind", "json-key");
    cJSON_AddStringToObject(change, "file", place->config);
    cJSON_AddItemToObject(change, "key", cJSON_Parse("[\"commands\",\"oc\"]"));
    cJSON_AddItemToObject(change, "value", cJSON_Parse("{\"enabled\":true}"));
    return succeeds(call(runtime_changes_apply_json, request), "json-key");
}

static int reverses_cleanly(const scratch *place, const char *host) {
    answer result = call(runtime_changes_reverse_json, host_request(place, host));
    int clean = result.status == SPISOR_STATUS_SUCCESS &&
                cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(result.tree, "drifted")) == 0;
    cJSON_Delete(result.tree);
    return clean;
}

static int forgets(const scratch *place, const char *host) {
    cJSON *request = places_request(place);
    cJSON_AddStringToObject(request, "id", host);
    return succeeds(call(runtime_registry_forget, request), "forget");
}

static int count_of_kind(const scratch *place, const char *host, const char *kind) {
    cJSON *registration = NULL;
    CHECK(runtime_registry_load(place->state, host, &registration) == RUNTIME_REGISTRY_OK);
    int count = 0;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(registration, "changes")) {
        const char *entry_kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "kind"));
        count += entry_kind != NULL && strcmp(entry_kind, kind) == 0;
    }
    cJSON_Delete(registration);
    return count;
}

static int has_shim(const scratch *place, const char *name) {
    char path[1400];
    snprintf(path, sizeof path, "%s/%s" PROGRAM_SUFFIX, place->shims, name);
    return runtime_probe_entry(path) == RUNTIME_ENTRY_OTHER;
}

static int file_is(const char *path, const char *expected) {
    size_t length = 0;
    char *bytes = (char *)spisor_read_file(path, &length);
    int same = check_text(bytes, expected, path);
    free(bytes);
    return same;
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
    value->present = RegGetValueW(HKEY_CURRENT_USER, ENVIRONMENT_KEY, name, READ_FLAGS, &value->type, value->text,
                                  &size) == ERROR_SUCCESS;
}

static void delete_test_value(void) {
    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, ENVIRONMENT_KEY, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, RUNTIME_PATH_ENTRY_TEST_VALUE);
        RegCloseKey(key);
    }
}

static int shims_on_path(const scratch *place) {
    char backslashed[1400];
    wchar_t wide[1400];
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

static void install_both_hosts(const scratch *place) {
    CHECK(registers(place, "alpha"));
    CHECK(requests(place, "alpha", "oc", "oc"));
    CHECK(requests(place, "alpha", "cc", "cc2"));
    CHECK(shims_on_path(place));
    CHECK(count_of_kind(place, "alpha", "path-entry") == 1);
#ifndef _WIN32
    CHECK(count_of_kind(place, "alpha", "profile-line") >= 1);
#endif
    CHECK(sets_application_key(place, "alpha"));
    CHECK(registers(place, "beta"));
    CHECK(requests(place, "beta", "bc", "bc"));
    CHECK(has_shim(place, "oc") && has_shim(place, "cc2") && has_shim(place, "bc"));
    CHECK(renames(place, "alpha", "oc", "open"));
    CHECK(deletes(place, "beta", "bc"));
    CHECK(has_shim(place, "open") && !has_shim(place, "oc") && !has_shim(place, "bc"));
}

static void remove_both_hosts(const scratch *place) {
    CHECK(reverses_cleanly(place, "alpha"));
    CHECK(forgets(place, "alpha"));
    CHECK(!has_shim(place, "open") && !has_shim(place, "cc2"));
    CHECK(runtime_probe_entry(place->shims) == RUNTIME_ENTRY_DIRECTORY);
    CHECK(shims_on_path(place));
    CHECK(reverses_cleanly(place, "beta"));
    CHECK(forgets(place, "beta"));
    CHECK(runtime_probe_entry(place->shims) == RUNTIME_ENTRY_MISSING);
    CHECK(!shims_on_path(place));
}

/** Whether the registry's own folder holds nothing but its lock file. */
static int registry_holds_only_its_lock(const scratch *place) {
    char hosts[1200];
    snprintf(hosts, sizeof hosts, "%s/hosts", place->state);
    runtime_directory_listing listing;
    CHECK(runtime_list_directory(hosts, &listing) == 1);
    int only_lock = listing.count == 0 || (listing.count == 1 && strcmp(listing.entries[0].name, ".lock") == 0);
    runtime_free_directory_listing(&listing);
    return only_lock;
}

/**
 * Proof 2 end to end: two hosts with commands, a collision, a shared state/bin, a user rename and
 * delete and a key in an application config, then both reversed and forgotten, leave the scratch
 * tree byte-identical to the one before.
 *
 * @implNote state/hosts is left out of the comparison and checked on its own, since the registry
 * keeps its lock file there for good.
 */
static void two_hosts_installed_and_removed_leave_the_tree_as_it_was(void) {
    scratch place;
    fresh_scratch(&place);
    seed_scratch(&place);
    char *before = tree_snapshot(place.directory, "/state/hosts");
    install_both_hosts(&place);
    remove_both_hosts(&place);
    char *after = tree_snapshot(place.directory, "/state/hosts");
    check_text(after, before, "the scratch tree after both hosts are gone");
    CHECK(registry_holds_only_its_lock(&place));
    char portable[1300];
    snprintf(portable, sizeof portable, "%s/config.json", place.portable);
    CHECK(file_is(portable, portable_bytes));
    CHECK(file_is(place.secrets, secrets_bytes));
    CHECK(file_is(place.config, application_config));
    free(before);
    free(after);
}

int main(void) {
#ifdef _WIN32
    registry_text real_before;
    read_value(L"Path", &real_before);
    delete_test_value();
#endif
    two_hosts_installed_and_removed_leave_the_tree_as_it_was();
#ifdef _WIN32
    delete_test_value();
    registry_text real_after;
    read_value(L"Path", &real_after);
    CHECK(real_before.present == real_after.present && real_before.type == real_after.type &&
          wcscmp(real_before.text, real_after.text) == 0);
#endif
    return check_summary("lifecycle_test");
}
