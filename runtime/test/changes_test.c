#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#define CURRENT_PROCESS_ID _getpid()
#else
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#define CURRENT_PROCESS_ID getpid()
#endif

#include "change_reversal_hooks.h"
#include "changes.h"
#include "check.h"
#include "host_provenance.h"
#include "scratch_directory.h"
#include "directory_tree.h"
#include "links.h"
#include "paths.h"
#include "registry.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"

typedef int32_t (*changes_export)(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

typedef struct scratch {
    char directory[1024];
    char home[1100];
    char state[1100];
    char cache[1100];
} scratch;

typedef struct answer {
    int32_t status;
    cJSON *tree;
} answer;

static const char *const claude_json_before = "{\n"
                                              "  \"theme\": \"dark\",\n"
                                              "  \"projects\": {\n"
                                              "    \"other\": {\n"
                                              "      \"allowedTools\": []\n"
                                              "    }\n"
                                              "  }\n"
                                              "}\n";

static const char *const path_line = "export PATH=\"/opt/spisor/bin:$PATH\"";

static void fresh_scratch(scratch *place, const char *label) {
    static int sequence = 0;
    snprintf(place->directory, sizeof place->directory, RUNTIME_SCRATCH_PATH "/changes-%d-%d-%s",
             (int)CURRENT_PROCESS_ID, sequence++, label);
    snprintf(place->home, sizeof place->home, "%s/home", place->directory);
    snprintf(place->state, sizeof place->state, "%s/state", place->directory);
    snprintf(place->cache, sizeof place->cache, "%s/cache", place->directory);
    start_empty_directory(place->directory);
    CHECK(spisor_make_directories(place->home) == 1);
    CHECK(spisor_make_directories(place->state) == 1);
}

static void in_home(const scratch *place, const char *relative, char *path, size_t size) {
    snprintf(path, size, "%s/%s", place->home, relative);
}

static void write_text(const char *path, const char *text) {
    CHECK(spisor_replace_file(path, (const uint8_t *)text, strlen(text)) == 1);
}

static void write_home_file(const scratch *place, const char *relative, const char *text) {
    char path[1400];
    in_home(place, relative, path, sizeof path);
    write_text(path, text);
}

/** The file's bytes as text, freed with free, or NULL when it cannot be read. */
static char *read_text(const char *path) {
    size_t length = 0;
    return (char *)spisor_read_file(path, &length);
}

static int home_file_is(const scratch *place, const char *relative, const char *expected) {
    char path[1400];
    in_home(place, relative, path, sizeof path);
    char *actual = read_text(path);
    int same = check_text(actual, expected, relative);
    free(actual);
    return same;
}

static int home_entry_exists(const scratch *place, const char *relative) {
    char path[1400];
    in_home(place, relative, path, sizeof path);
    return runtime_probe_entry(path) != RUNTIME_ENTRY_MISSING;
}

static void append_snapshot(const char *directory, const char *relative, char **snapshot, size_t *length);

static void append_text(char **snapshot, size_t *length, const char *text, size_t text_length) {
    char *grown = realloc(*snapshot, *length + text_length + 1);
    CHECK(grown != NULL);
    if (grown == NULL) {
        return;
    }
    memcpy(grown + *length, text, text_length);
    *length += text_length;
    grown[*length] = '\0';
    *snapshot = grown;
}

static void append_entry(const char *directory, const char *relative, const runtime_directory_entry *entry,
                         char **snapshot, size_t *length) {
    char path[2048];
    char name[2048];
    snprintf(path, sizeof path, "%s/%s", directory, entry->name);
    snprintf(name, sizeof name, "%s/%s", relative, entry->name);
    char header[2200];
    if (entry->is_directory) {
        snprintf(header, sizeof header, "directory %s\n", name);
        append_text(snapshot, length, header, strlen(header));
        append_snapshot(path, name, snapshot, length);
        return;
    }
    size_t content_length = 0;
    uint8_t *content = spisor_read_file(path, &content_length);
    snprintf(header, sizeof header, "file %s %zu\n", name, content_length);
    append_text(snapshot, length, header, strlen(header));
    if (content != NULL) {
        append_text(snapshot, length, (const char *)content, content_length);
    }
    free(content);
}

static void append_snapshot(const char *directory, const char *relative, char **snapshot, size_t *length) {
    runtime_directory_listing listing;
    CHECK(runtime_list_directory(directory, &listing) == 1);
    for (size_t index = 0; index < listing.count; index++) {
        append_entry(directory, relative, &listing.entries[index], snapshot, length);
    }
    runtime_free_directory_listing(&listing);
}

/** Every name, kind and byte under the scratch home, freed with free. */
static char *snapshot_of(const scratch *place) {
    char *snapshot = NULL;
    size_t length = 0;
    append_text(&snapshot, &length, "", 0);
    append_snapshot(place->home, "", &snapshot, &length);
    return snapshot;
}

static cJSON *places_for(const scratch *place) {
    cJSON *places = cJSON_CreateObject();
    cJSON_AddStringToObject(places, "state", place->state);
    cJSON_AddStringToObject(places, "cache", place->cache);
    return places;
}

static answer call(changes_export function, cJSON *request) {
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

static void register_host_at(const scratch *place, const char *id, const char *location) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddItemToObject(request, "places", places_for(place));
    cJSON *registration = cJSON_AddObjectToObject(request, "registration");
    cJSON_AddStringToObject(registration, "id", id);
    cJSON_AddStringToObject(registration, "kind", "host");
    cJSON_AddStringToObject(registration, "version", "1.0.0");
    add_host_provenance(registration, "npm", "1.0.0", location);
    cJSON_AddBoolToObject(registration, "requiresCommand", 0);
    answer result = call(runtime_registry_register, request);
    CHECK(result.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(result.tree);
}

static void register_host(const scratch *place, const char *id) {
    register_host_at(place, id, place->directory);
}

static cJSON *host_request(const scratch *place, const char *host) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddItemToObject(request, "places", places_for(place));
    cJSON_AddStringToObject(request, "host", host);
    return request;
}

static answer apply_change(const scratch *place, const char *host, cJSON *change) {
    cJSON *request = host_request(place, host);
    cJSON_AddItemToObject(request, "change", change);
    return call(runtime_changes_apply_json, request);
}

/** The sequence apply recorded, or -1 when it refused or recorded nothing. */
static long applied_sequence(const scratch *place, const char *host, cJSON *change) {
    answer result = apply_change(place, host, change);
    const cJSON *sequence = cJSON_GetObjectItemCaseSensitive(result.tree, "sequence");
    long recorded = result.status == SPISOR_STATUS_SUCCESS &&
                            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result.tree, "recorded")) &&
                            cJSON_IsNumber(sequence)
                        ? (long)sequence->valuedouble
                        : -1;
    if (recorded < 0) {
        char *text = result.tree == NULL ? NULL : cJSON_PrintUnformatted(result.tree);
        fprintf(stderr, "apply did not record: status %d %s\n", (int)result.status, text == NULL ? "" : text);
        free(text);
    }
    cJSON_Delete(result.tree);
    return recorded;
}

static int apply_refused(const scratch *place, const char *host, cJSON *change) {
    answer result = apply_change(place, host, change);
    int refused = result.status == SPISOR_STATUS_FAILURE &&
                  cJSON_IsString(cJSON_GetObjectItemCaseSensitive(result.tree, "message"));
    cJSON_Delete(result.tree);
    return refused;
}

static int apply_unneeded(const scratch *place, const char *host, cJSON *change) {
    answer result = apply_change(place, host, change);
    int unneeded = result.status == SPISOR_STATUS_SUCCESS &&
                   cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(result.tree, "recorded")) &&
                   cJSON_GetObjectItemCaseSensitive(result.tree, "sequence") == NULL;
    cJSON_Delete(result.tree);
    return unneeded;
}

/** The answer of reverse, freed with cJSON_Delete; NULL when it did not succeed. */
static cJSON *reverse_host(const scratch *place, const char *host) {
    answer result = call(runtime_changes_reverse_json, host_request(place, host));
    if (result.status != SPISOR_STATUS_SUCCESS) {
        char *text = result.tree == NULL ? NULL : cJSON_PrintUnformatted(result.tree);
        fprintf(stderr, "reverse failed: status %d %s\n", (int)result.status, text == NULL ? "" : text);
        free(text);
        cJSON_Delete(result.tree);
        return NULL;
    }
    return result.tree;
}

static int reversed_count(const cJSON *result) {
    const cJSON *reversed = cJSON_GetObjectItemCaseSensitive(result, "reversed");
    return cJSON_IsNumber(reversed) ? reversed->valueint : -1;
}

static int drift_count(const cJSON *result) {
    const cJSON *drifted = cJSON_GetObjectItemCaseSensitive(result, "drifted");
    return cJSON_IsArray(drifted) ? cJSON_GetArraySize(drifted) : -1;
}

static int reverse_counts(const scratch *place, const char *host, int expected_reversed, int expected_drifted) {
    cJSON *result = reverse_host(place, host);
    int matches = result != NULL && reversed_count(result) == expected_reversed &&
                  drift_count(result) == expected_drifted;
    if (!matches) {
        fprintf(stderr, "reverse of %s: expected %d reversed and %d drifted, got %d and %d\n", host,
                expected_reversed, expected_drifted, reversed_count(result), drift_count(result));
    }
    cJSON_Delete(result);
    return matches;
}

static cJSON *path_change(const char *kind, const scratch *place, const char *relative) {
    char path[1400];
    in_home(place, relative, path, sizeof path);
    cJSON *change = cJSON_CreateObject();
    cJSON_AddStringToObject(change, "kind", kind);
    cJSON_AddStringToObject(change, "path", path);
    return change;
}

static cJSON *directory_change(const scratch *place, const char *relative) {
    return path_change("directory", place, relative);
}

static cJSON *file_change(const scratch *place, const char *relative, const char *content, int executable) {
    cJSON *change = path_change("file", place, relative);
    cJSON_AddStringToObject(change, "content", content);
    cJSON_AddBoolToObject(change, "executable", executable);
    return change;
}

static cJSON *file_named_change(const char *kind, const scratch *place, const char *relative) {
    char path[1400];
    in_home(place, relative, path, sizeof path);
    cJSON *change = cJSON_CreateObject();
    cJSON_AddStringToObject(change, "kind", kind);
    cJSON_AddStringToObject(change, "file", path);
    return change;
}

static cJSON *json_key_change(const scratch *place, const char *key_json, const char *value_json) {
    cJSON *change = file_named_change("json-key", place, ".claude.json");
    cJSON_AddItemToObject(change, "key", cJSON_Parse(key_json));
    cJSON_AddItemToObject(change, "value", cJSON_Parse(value_json));
    return change;
}

static cJSON *profile_line_change(const scratch *place, const char *relative, const char *line) {
    cJSON *change = file_named_change("profile-line", place, relative);
    cJSON_AddStringToObject(change, "line", line);
    return change;
}

static cJSON *loaded_registration(const scratch *place, const char *host) {
    cJSON *registration = NULL;
    CHECK(runtime_registry_load(place->state, host, &registration) == RUNTIME_REGISTRY_OK);
    return registration;
}

static int change_count(const scratch *place, const char *host) {
    cJSON *registration = loaded_registration(place, host);
    int count = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(registration, "changes"));
    cJSON_Delete(registration);
    return count;
}

static int is_change_reversed(const scratch *place, const char *host, long sequence) {
    cJSON *registration = loaded_registration(place, host);
    int reversed = -1;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(registration, "changes")) {
        if ((long)cJSON_GetObjectItemCaseSensitive(entry, "sequence")->valuedouble == sequence) {
            reversed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "reversed"));
        }
    }
    cJSON_Delete(registration);
    return reversed;
}

static int forgets(const scratch *place, const char *host) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddItemToObject(request, "places", places_for(place));
    cJSON_AddStringToObject(request, "id", host);
    answer result = call(runtime_registry_forget, request);
    int forgotten = result.status == SPISOR_STATUS_SUCCESS &&
                    cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result.tree, "forgotten"));
    cJSON_Delete(result.tree);
    return forgotten;
}

static void seed_home(const scratch *place) {
    write_home_file(place, ".claude.json", claude_json_before);
    write_home_file(place, ".bashrc", "export A=1\n");
    write_home_file(place, ".zshrc", "alias ll='ls -l'");
}

static int is_owner_executable(const scratch *place, const char *relative) {
#ifdef _WIN32
    (void)place;
    (void)relative;
    return 1;
#else
    char path[1400];
    in_home(place, relative, path, sizeof path);
    struct stat status;
    return stat(path, &status) == 0 && (status.st_mode & S_IXUSR) != 0;
#endif
}

static void apply_every_kind(const scratch *place) {
    CHECK(applied_sequence(place, "alpha", directory_change(place, "tools")) == 1);
    CHECK(applied_sequence(place, "alpha", file_change(place, "tools/hello", "#!/bin/sh\necho hi\n", 1)) == 2);
    CHECK(applied_sequence(place, "alpha", json_key_change(place, "[\"projects\",\"x\",\"allowedTools\"]",
                                                            "[\"Bash\"]")) == 3);
    CHECK(applied_sequence(place, "alpha", json_key_change(place, "[\"theme\"]", "\"light\"")) == 4);
    CHECK(applied_sequence(place, "alpha", json_key_change(place, "[\"fresh\",\"deep\"]", "{\"a\":1}")) == 5);
    CHECK(applied_sequence(place, "alpha", profile_line_change(place, ".bashrc", path_line)) == 6);
    CHECK(applied_sequence(place, "alpha", profile_line_change(place, ".zshrc", path_line)) == 7);
}

static void check_every_kind_applied(const scratch *place) {
    home_file_is(place, "tools/hello", "#!/bin/sh\necho hi\n");
    CHECK(is_owner_executable(place, "tools/hello"));
    home_file_is(place, ".claude.json",
                 "{\n  \"theme\": \"light\",\n  \"projects\": {\n    \"other\": {\n      \"allowedTools\": []\n"
                 "    },\n    \"x\": {\n      \"allowedTools\": [\n        \"Bash\"\n      ]\n    }\n  },\n"
                 "  \"fresh\": {\n    \"deep\": {\n      \"a\": 1\n    }\n  }\n}\n");
    home_file_is(place, ".bashrc", "export A=1\nexport PATH=\"/opt/spisor/bin:$PATH\"\n");
    home_file_is(place, ".zshrc", "alias ll='ls -l'\nexport PATH=\"/opt/spisor/bin:$PATH\"\n");
}

static void each_kind_reverses_to_the_bytes_before(void) {
    scratch place;
    fresh_scratch(&place, "every-kind");
    register_host(&place, "alpha");
    seed_home(&place);
    char *before = snapshot_of(&place);
    apply_every_kind(&place);
    check_every_kind_applied(&place);
    CHECK(reverse_counts(&place, "alpha", 7, 0));
    char *after = snapshot_of(&place);
    check_text(after, before, "home after reverse");
    for (long sequence = 1; sequence <= 7; sequence++) {
        CHECK(is_change_reversed(&place, "alpha", sequence) == 1);
    }
    CHECK(reverse_counts(&place, "alpha", 0, 0));
    CHECK(forgets(&place, "alpha"));
    free(before);
    free(after);
}

static const char *const user_edited_claude_json = "{\n"
                                                   "  \"theme\": \"dark\",\n"
                                                   "  \"projects\": {\n"
                                                   "    \"other\": {\n"
                                                   "      \"allowedTools\": []\n"
                                                   "    },\n"
                                                   "    \"x\": {\n"
                                                   "      \"allowedTools\": [\n"
                                                   "        \"Read\"\n"
                                                   "      ]\n"
                                                   "    }\n"
                                                   "  }\n"
                                                   "}\n";

static void check_drift_report(const cJSON *result) {
    const cJSON *report = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(result, "drifted"), 0);
    const cJSON *sequence = cJSON_GetObjectItemCaseSensitive(report, "sequence");
    const char *detail = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(report, "detail"));
    CHECK(cJSON_IsNumber(sequence) && sequence->valueint == 1);
    check_text(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(report, "kind")), "json-key", "drift kind");
    CHECK(detail != NULL && strstr(detail, "projects.x.allowedTools") != NULL);
}

static void a_key_the_user_changed_is_left_and_reported(void) {
    scratch place;
    fresh_scratch(&place, "drift");
    register_host(&place, "alpha");
    seed_home(&place);
    CHECK(applied_sequence(&place, "alpha",
                           json_key_change(&place, "[\"projects\",\"x\",\"allowedTools\"]", "[\"Bash\"]")) == 1);
    write_home_file(&place, ".claude.json", user_edited_claude_json);
    cJSON *result = reverse_host(&place, "alpha");
    CHECK(reversed_count(result) == 1 && drift_count(result) == 1);
    check_drift_report(result);
    cJSON_Delete(result);
    home_file_is(&place, ".claude.json", user_edited_claude_json);
    CHECK(is_change_reversed(&place, "alpha", 1) == 1);
}

static void an_interrupted_removal_finishes_and_reverses_nothing_twice(void) {
    scratch place;
    fresh_scratch(&place, "interrupted");
    register_host(&place, "alpha");
    CHECK(applied_sequence(&place, "alpha", directory_change(&place, "tools")) == 1);
    CHECK(applied_sequence(&place, "alpha", file_change(&place, "tools/a", "a", 0)) == 2);
    CHECK(applied_sequence(&place, "alpha", file_change(&place, "tools/b", "b", 0)) == 3);
    CHECK(applied_sequence(&place, "alpha", file_change(&place, "tools/c", "c", 0)) == 4);
    runtime_changes_allow_reversals(2);
    CHECK(reverse_counts(&place, "alpha", 2, 0));
    runtime_changes_allow_reversals(-1);
    CHECK(is_change_reversed(&place, "alpha", 4) == 1 && is_change_reversed(&place, "alpha", 3) == 1);
    CHECK(is_change_reversed(&place, "alpha", 2) == 0 && is_change_reversed(&place, "alpha", 1) == 0);
    CHECK(!home_entry_exists(&place, "tools/c") && home_entry_exists(&place, "tools/a"));
    write_home_file(&place, "tools/c", "the user's own c");
    CHECK(reverse_counts(&place, "alpha", 2, 1));
    home_file_is(&place, "tools/c", "the user's own c");
    CHECK(!home_entry_exists(&place, "tools/a") && home_entry_exists(&place, "tools"));
    CHECK(reverse_counts(&place, "alpha", 0, 0));
    CHECK(forgets(&place, "alpha"));
}

static void a_shared_line_and_directory_stay_until_both_hosts_are_reversed(void) {
    scratch place;
    fresh_scratch(&place, "shared");
    register_host(&place, "alpha");
    register_host(&place, "beta");
    seed_home(&place);
    char *before = snapshot_of(&place);
    CHECK(applied_sequence(&place, "alpha", profile_line_change(&place, ".bashrc", path_line)) == 1);
    CHECK(applied_sequence(&place, "beta", profile_line_change(&place, ".bashrc", path_line)) == 1);
    CHECK(applied_sequence(&place, "alpha", directory_change(&place, "bin")) == 2);
    CHECK(applied_sequence(&place, "beta", directory_change(&place, "bin")) == 2);
    home_file_is(&place, ".bashrc", "export A=1\nexport PATH=\"/opt/spisor/bin:$PATH\"\n");
    CHECK(reverse_counts(&place, "alpha", 2, 0));
    home_file_is(&place, ".bashrc", "export A=1\nexport PATH=\"/opt/spisor/bin:$PATH\"\n");
    CHECK(home_entry_exists(&place, "bin"));
    CHECK(reverse_counts(&place, "beta", 2, 0));
    char *after = snapshot_of(&place);
    check_text(after, before, "home after both hosts reversed");
    free(before);
    free(after);
}

static void an_unreadable_json_file_is_refused_and_untouched(void) {
    scratch place;
    fresh_scratch(&place, "unreadable");
    register_host(&place, "alpha");
    write_home_file(&place, ".claude.json", "{ not json");
    CHECK(apply_refused(&place, "alpha", json_key_change(&place, "[\"theme\"]", "\"light\"")));
    home_file_is(&place, ".claude.json", "{ not json");
    write_home_file(&place, ".claude.json", "[1, 2]\n");
    CHECK(apply_refused(&place, "alpha", json_key_change(&place, "[\"theme\"]", "\"light\"")));
    home_file_is(&place, ".claude.json", "[1, 2]\n");
    write_home_file(&place, ".claude.json", "{\"theme\": 3}\n");
    CHECK(apply_refused(&place, "alpha", json_key_change(&place, "[\"theme\",\"name\"]", "\"light\"")));
    home_file_is(&place, ".claude.json", "{\"theme\": 3}\n");
    CHECK(change_count(&place, "alpha") == 0);
}

static void reverse_needs_nothing_but_the_record(void) {
    scratch place;
    fresh_scratch(&place, "orphan");
    char location[1400];
    snprintf(location, sizeof location, "%s/installed-host", place.directory);
    write_text(location, "the host's binary");
    register_host_at(&place, "alpha", location);
    CHECK(applied_sequence(&place, "alpha", file_change(&place, "shim", "shim", 1)) == 1);
    CHECK(remove(location) == 0);
    CHECK(reverse_counts(&place, "alpha", 1, 0));
    CHECK(!home_entry_exists(&place, "shim"));
    CHECK(forgets(&place, "alpha"));
}

static cJSON *pending_file_entry(long sequence, const char *path) {
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddNumberToObject(entry, "sequence", (double)sequence);
    cJSON_AddStringToObject(entry, "kind", "file");
    cJSON_AddStringToObject(entry, "state", "pending");
    cJSON_AddFalseToObject(entry, "reversed");
    cJSON_AddStringToObject(entry, "path", path);
    return entry;
}

static void a_pending_record_is_completed_or_dropped_from_the_disk(void) {
    scratch place;
    fresh_scratch(&place, "pending");
    register_host(&place, "alpha");
    char made[1400];
    char never_made[1400];
    in_home(&place, "made", made, sizeof made);
    in_home(&place, "never-made", never_made, sizeof never_made);
    write_text(made, "made before the crash");
    cJSON *registration = loaded_registration(&place, "alpha");
    cJSON *changes = cJSON_GetObjectItemCaseSensitive(registration, "changes");
    cJSON_AddItemToArray(changes, pending_file_entry(1, made));
    cJSON_AddItemToArray(changes, pending_file_entry(2, never_made));
    CHECK(runtime_registry_store(place.state, registration) == RUNTIME_REGISTRY_OK);
    cJSON_Delete(registration);
    CHECK(reverse_counts(&place, "alpha", 1, 0));
    CHECK(!home_entry_exists(&place, "made"));
    CHECK(change_count(&place, "alpha") == 1);
    CHECK(is_change_reversed(&place, "alpha", 1) == 1);
}

static void refusals_change_nothing(void) {
    scratch place;
    fresh_scratch(&place, "refusals");
    register_host(&place, "alpha");
    seed_home(&place);
    write_home_file(&place, "taken", "the user's file");
    CHECK(apply_refused(&place, "alpha", file_change(&place, "taken", "the host's", 0)));
    home_file_is(&place, "taken", "the user's file");
    cJSON *relative = cJSON_CreateObject();
    cJSON_AddStringToObject(relative, "kind", "directory");
    cJSON_AddStringToObject(relative, "path", "relative/tools");
    CHECK(apply_refused(&place, "alpha", relative));
    cJSON *unknown = path_change("registry-key", &place, "anything");
    CHECK(apply_refused(&place, "alpha", unknown));
    CHECK(apply_refused(&place, "alpha", profile_line_change(&place, ".bashrc", "two\nlines")));
    CHECK(apply_refused(&place, "alpha", profile_line_change(&place, ".missing-profile", path_line)));
    CHECK(apply_refused(&place, "alpha", file_change(&place, "no-such-directory/file", "x", 0)));
    CHECK(apply_refused(&place, "nobody", directory_change(&place, "tools")));
    CHECK(!home_entry_exists(&place, "tools") && !home_entry_exists(&place, "no-such-directory"));
    CHECK(change_count(&place, "alpha") == 0);
}

static void what_the_user_already_has_is_not_recorded(void) {
    scratch place;
    fresh_scratch(&place, "unneeded");
    register_host(&place, "alpha");
    seed_home(&place);
    char existing[1400];
    in_home(&place, "existing", existing, sizeof existing);
    CHECK(spisor_make_directories(existing) == 1);
    CHECK(apply_unneeded(&place, "alpha", directory_change(&place, "existing")));
    CHECK(apply_unneeded(&place, "alpha", profile_line_change(&place, ".bashrc", "export A=1")));
    CHECK(apply_unneeded(&place, "alpha", json_key_change(&place, "[\"theme\"]", "\"dark\"")));
    CHECK(change_count(&place, "alpha") == 0);
    CHECK(reverse_counts(&place, "alpha", 0, 0));
    CHECK(home_entry_exists(&place, "existing"));
    home_file_is(&place, ".bashrc", "export A=1\n");
    home_file_is(&place, ".claude.json", claude_json_before);
}

static void a_sequence_is_never_handed_out_twice(void) {
    scratch place;
    fresh_scratch(&place, "sequence");
    register_host(&place, "alpha");
    CHECK(apply_refused(&place, "alpha", file_change(&place, "no-such-directory/file", "x", 0)));
    CHECK(change_count(&place, "alpha") == 0);
    register_host(&place, "alpha");
    CHECK(applied_sequence(&place, "alpha", directory_change(&place, "tools")) == 2);
}

static void other_spelling(const char *path, char *variant, size_t size) {
#ifdef _WIN32
    size_t index = 0;
    for (; path[index] != '\0' && index + 2 < size; index++) {
        char character = path[index] == '/' ? '\\' : path[index];
        variant[index] = character >= 'a' && character <= 'z' ? (char)(character - 'a' + 'A') : character;
    }
    variant[index] = '\\';
    variant[index + 1] = '\0';
#else
    snprintf(variant, size, "%s//", path);
#endif
}

static const char *stored_path(const cJSON *registration, int index) {
    const cJSON *entry = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(registration, "changes"), index);
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "path"));
}

static void two_spellings_of_one_path_are_one_target(void) {
    scratch place;
    fresh_scratch(&place, "spelling");
    register_host(&place, "alpha");
    register_host(&place, "beta");
    char path[1400];
    char variant[1400];
    in_home(&place, "shared", path, sizeof path);
    other_spelling(path, variant, sizeof variant);
    CHECK(applied_sequence(&place, "alpha", directory_change(&place, "shared")) == 1);
    cJSON *change = cJSON_CreateObject();
    cJSON_AddStringToObject(change, "kind", "directory");
    cJSON_AddStringToObject(change, "path", variant);
    CHECK(applied_sequence(&place, "beta", change) == 1);
    cJSON *registration = loaded_registration(&place, "beta");
    const char *stored = stored_path(registration, 0);
    size_t length = stored == NULL ? 0 : strlen(stored);
    CHECK(length > 0 && stored[length - 1] != '/' && stored[length - 1] != '\\');
    CHECK(RUNTIME_HOST_IS_WINDOWS ? strchr(stored, '/') == NULL : strstr(stored, "//") == NULL);
    cJSON_Delete(registration);
    CHECK(reverse_counts(&place, "alpha", 1, 0));
    CHECK(home_entry_exists(&place, "shared"));
    CHECK(reverse_counts(&place, "beta", 1, 0));
    CHECK(!home_entry_exists(&place, "shared"));
}

typedef enum host_action { APPLY_LINE, REVERSE, REGISTER_REPEATEDLY, APPLY_FILES } host_action;

#define RACING_ROUNDS 30

typedef struct host_work {
    const scratch *place;
    const char *host;
    host_action action;
    int succeeded;
} host_work;

static void apply_files(host_work *work) {
    work->succeeded = 1;
    for (int round = 0; round < RACING_ROUNDS; round++) {
        char name[64];
        snprintf(name, sizeof name, "racing-%d", round);
        work->succeeded &= applied_sequence(work->place, work->host, file_change(work->place, name, "x", 0)) > 0;
    }
}

static void register_repeatedly(host_work *work) {
    for (int round = 0; round < RACING_ROUNDS; round++) {
        register_host(work->place, work->host);
    }
    work->succeeded = 1;
}

static void perform_work(host_work *work) {
    cJSON *reversed = NULL;
    switch (work->action) {
    case APPLY_LINE:
        work->succeeded =
            applied_sequence(work->place, work->host, profile_line_change(work->place, ".bashrc", path_line)) > 0;
        break;
    case REVERSE:
        reversed = reverse_host(work->place, work->host);
        work->succeeded = reversed != NULL;
        cJSON_Delete(reversed);
        break;
    case REGISTER_REPEATEDLY:
        register_repeatedly(work);
        break;
    case APPLY_FILES:
        apply_files(work);
        break;
    }
}

#ifdef _WIN32
static DWORD WINAPI work_entry(LPVOID argument) {
    perform_work(argument);
    return 0;
}

static void run_together(host_work *first, host_work *second) {
    HANDLE handles[2];
    handles[0] = CreateThread(NULL, 0, work_entry, first, 0, NULL);
    handles[1] = CreateThread(NULL, 0, work_entry, second, 0, NULL);
    CHECK(handles[0] != NULL && handles[1] != NULL);
    for (int index = 0; index < 2; index++) {
        if (handles[index] != NULL) {
            WaitForSingleObject(handles[index], INFINITE);
            CloseHandle(handles[index]);
        }
    }
}
#else
static void *work_entry(void *argument) {
    perform_work(argument);
    return NULL;
}

static void run_together(host_work *first, host_work *second) {
    pthread_t threads[2];
    CHECK(pthread_create(&threads[0], NULL, work_entry, first) == 0);
    CHECK(pthread_create(&threads[1], NULL, work_entry, second) == 0);
    pthread_join(threads[0], NULL);
    pthread_join(threads[1], NULL);
}
#endif

static int both_together(const scratch *place, host_action alpha_action, host_action beta_action) {
    host_work alpha = {place, "alpha", alpha_action, 0};
    host_work beta = {place, "beta", beta_action, 0};
    run_together(&alpha, &beta);
    return alpha.succeeded && beta.succeeded;
}

static int line_occurrences(const scratch *place) {
    char path[1400];
    in_home(place, ".bashrc", path, sizeof path);
    char *text = read_text(path);
    int count = 0;
    for (const char *cursor = text; cursor != NULL && (cursor = strstr(cursor, path_line)) != NULL; cursor++) {
        count++;
    }
    free(text);
    return count;
}

static int race_round(const scratch *place) {
    int held_right = both_together(place, APPLY_LINE, APPLY_LINE) && line_occurrences(place) == 1;
    held_right &= both_together(place, REVERSE, REVERSE) && line_occurrences(place) == 0;
    host_work alpha = {place, "alpha", APPLY_LINE, 0};
    perform_work(&alpha);
    held_right &= alpha.succeeded && both_together(place, REVERSE, APPLY_LINE) && line_occurrences(place) == 1;
    host_work beta = {place, "beta", REVERSE, 0};
    perform_work(&beta);
    return held_right && beta.succeeded && line_occurrences(place) == 0;
}

static int listed_host_count(const scratch *place) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddItemToObject(request, "places", places_for(place));
    answer result = call(runtime_registry_list, request);
    const cJSON *hosts = cJSON_GetObjectItemCaseSensitive(result.tree, "hosts");
    const cJSON *unreadable = cJSON_GetObjectItemCaseSensitive(result.tree, "unreadable");
    int count = cJSON_GetArraySize(unreadable) == 0 ? cJSON_GetArraySize(hosts) : -1;
    cJSON_Delete(result.tree);
    return count;
}

static void hosts_racing_on_a_shared_line_keep_it_exactly_while_held(void) {
    scratch place;
    fresh_scratch(&place, "racing-line");
    register_host(&place, "alpha");
    register_host(&place, "beta");
    seed_home(&place);
    int rounds_right = 0;
    for (int round = 0; round < RACING_ROUNDS; round++) {
        rounds_right += race_round(&place);
    }
    if (rounds_right != RACING_ROUNDS) {
        fprintf(stderr, "racing hosts: %d of %d rounds kept the line exactly while held\n", rounds_right,
                RACING_ROUNDS);
    }
    CHECK(rounds_right == RACING_ROUNDS);
    home_file_is(&place, ".bashrc", "export A=1\n");
    CHECK(listed_host_count(&place) == 2);
}

static void registering_while_applying_never_drops_a_record(void) {
    scratch place;
    fresh_scratch(&place, "racing-register");
    register_host(&place, "alpha");
    host_work registering = {&place, "alpha", REGISTER_REPEATEDLY, 0};
    host_work applying = {&place, "alpha", APPLY_FILES, 0};
    run_together(&registering, &applying);
    CHECK(registering.succeeded && applying.succeeded);
    CHECK(change_count(&place, "alpha") == RACING_ROUNDS);
}

static int answered_existing(const scratch *place, const char *host, cJSON *change, long expected) {
    answer result = apply_change(place, host, change);
    const cJSON *sequence = cJSON_GetObjectItemCaseSensitive(result.tree, "sequence");
    int answered = result.status == SPISOR_STATUS_SUCCESS &&
                   cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(result.tree, "recorded")) &&
                   cJSON_IsNumber(sequence) && (long)sequence->valuedouble == expected;
    if (!answered) {
        char *text = result.tree == NULL ? NULL : cJSON_PrintUnformatted(result.tree);
        fprintf(stderr, "expected recorded false with sequence %ld, got %s\n", expected, text == NULL ? "" : text);
        free(text);
    }
    cJSON_Delete(result.tree);
    return answered;
}

static void a_repeated_change_answers_its_record_and_records_nothing(void) {
    scratch place;
    fresh_scratch(&place, "repeat");
    register_host(&place, "alpha");
    write_home_file(&place, ".claude.json", claude_json_before);
    write_home_file(&place, ".bashrc", "# bash\n");
    char *before = snapshot_of(&place);
    CHECK(applied_sequence(&place, "alpha", directory_change(&place, "tools")) == 1);
    CHECK(applied_sequence(&place, "alpha", file_change(&place, "tools/c", "#!/bin/sh\n", 1)) == 2);
    CHECK(applied_sequence(&place, "alpha", json_key_change(&place, "[\"theme\"]", "\"light\"")) == 3);
    CHECK(applied_sequence(&place, "alpha", profile_line_change(&place, ".bashrc", path_line)) == 4);
    char *applied = snapshot_of(&place);
    CHECK(answered_existing(&place, "alpha", directory_change(&place, "tools"), 1));
    CHECK(answered_existing(&place, "alpha", file_change(&place, "tools/c", "#!/bin/sh\n", 1), 2));
    CHECK(answered_existing(&place, "alpha", json_key_change(&place, "[\"theme\"]", "\"light\""), 3));
    CHECK(answered_existing(&place, "alpha", profile_line_change(&place, ".bashrc", path_line), 4));
    CHECK(change_count(&place, "alpha") == 4);
    char *repeated = snapshot_of(&place);
    CHECK(check_text(repeated, applied, "home after the repeats"));
    CHECK(reverse_counts(&place, "alpha", 4, 0));
    char *after = snapshot_of(&place);
    CHECK(check_text(after, before, "home after one reverse"));
    free(before);
    free(applied);
    free(repeated);
    free(after);
}

static void a_file_asked_for_new_content_is_rewritten_under_its_record(void) {
    scratch place;
    fresh_scratch(&place, "new-content");
    register_host(&place, "alpha");
    register_host(&place, "beta");
    char *before = snapshot_of(&place);
    CHECK(applied_sequence(&place, "alpha", file_change(&place, "shim", "run one\n", 1)) == 1);
    CHECK(answered_existing(&place, "alpha", file_change(&place, "shim", "run two\n", 1), 1));
    CHECK(home_file_is(&place, "shim", "run two\n"));
    CHECK(is_owner_executable(&place, "shim"));
    CHECK(answered_existing(&place, "alpha", file_change(&place, "shim", "run two\n", 1), 1));
    CHECK(home_file_is(&place, "shim", "run two\n"));
    CHECK(change_count(&place, "alpha") == 1);
    CHECK(apply_refused(&place, "beta", file_change(&place, "shim", "run three\n", 1)));
    CHECK(home_file_is(&place, "shim", "run two\n"));
    char path[1400];
    in_home(&place, "shim", path, sizeof path);
    CHECK(remove(path) == 0);
    CHECK(answered_existing(&place, "alpha", file_change(&place, "shim", "run four\n", 1), 1));
    CHECK(home_file_is(&place, "shim", "run four\n"));
    CHECK(reverse_counts(&place, "alpha", 1, 0));
    char *after = snapshot_of(&place);
    CHECK(check_text(after, before, "home after reversing the rewritten file"));
    free(before);
    free(after);
}

static void the_c_entry_point_records_like_the_json_one(void) {
    scratch place;
    fresh_scratch(&place, "c-entry");
    register_host(&place, "alpha");
    cJSON *change = directory_change(&place, "bin");
    runtime_changes_outcome outcome = runtime_changes_apply(place.state, "alpha", change);
    CHECK(outcome.answer == RUNTIME_CHANGES_RECORDED && outcome.sequence == 1);
    outcome = runtime_changes_apply(place.state, "alpha", change);
    CHECK(outcome.answer == RUNTIME_CHANGES_UNNEEDED && outcome.sequence == 1);
    cJSON_Delete(change);
    cJSON *taken = file_change(&place, "bin", "x", 0);
    outcome = runtime_changes_apply(place.state, "alpha", taken);
    CHECK(outcome.answer == RUNTIME_CHANGES_REFUSED && outcome.reason[0] != '\0');
    cJSON_Delete(taken);
    CHECK(reverse_counts(&place, "alpha", 1, 0));
    CHECK(!home_entry_exists(&place, "bin"));
}

static void a_link_left_where_a_file_was_is_removed_never_followed(void) {
    scratch place;
    fresh_scratch(&place, "link");
    register_host(&place, "alpha");
    char sentinel[1400];
    char shim[1400];
    snprintf(sentinel, sizeof sentinel, "%s/sentinel", place.directory);
    in_home(&place, "shim", shim, sizeof shim);
    write_text(sentinel, "outside the home");
    CHECK(applied_sequence(&place, "alpha", file_change(&place, "shim", "shim", 0)) == 1);
    CHECK(remove(shim) == 0);
    if (!create_link_or_skip(shim, sentinel)) {
        return;
    }
    CHECK(reverse_counts(&place, "alpha", 1, 0));
    CHECK(!home_entry_exists(&place, "shim"));
    char *kept = read_text(sentinel);
    check_text(kept, "outside the home", "link target");
    free(kept);
}

static void an_appended_line_is_crlf_only_when_every_line_of_the_profile_is(void) {
    scratch place;
    fresh_scratch(&place, "line-endings");
    register_host(&place, "alpha");
    write_home_file(&place, ".bashrc", "export A=1\nexport B=2\r\nexport C=3\n");
    write_home_file(&place, ".zshrc", "export A=1\r\nexport B=2\r\n");
    char *before = snapshot_of(&place);
    CHECK(applied_sequence(&place, "alpha", profile_line_change(&place, ".bashrc", path_line)) == 1);
    CHECK(applied_sequence(&place, "alpha", profile_line_change(&place, ".zshrc", path_line)) == 2);
    home_file_is(&place, ".bashrc", "export A=1\nexport B=2\r\nexport C=3\nexport PATH=\"/opt/spisor/bin:$PATH\"\n");
    home_file_is(&place, ".zshrc", "export A=1\r\nexport B=2\r\nexport PATH=\"/opt/spisor/bin:$PATH\"\r\n");
    CHECK(reverse_counts(&place, "alpha", 2, 0));
    char *after = snapshot_of(&place);
    check_text(after, before, "home after reversing the lines");
    free(before);
    free(after);
}

int main(void) {
    each_kind_reverses_to_the_bytes_before();
    a_key_the_user_changed_is_left_and_reported();
    an_interrupted_removal_finishes_and_reverses_nothing_twice();
    a_shared_line_and_directory_stay_until_both_hosts_are_reversed();
    an_unreadable_json_file_is_refused_and_untouched();
    reverse_needs_nothing_but_the_record();
    a_pending_record_is_completed_or_dropped_from_the_disk();
    refusals_change_nothing();
    what_the_user_already_has_is_not_recorded();
    a_sequence_is_never_handed_out_twice();
    two_spellings_of_one_path_are_one_target();
    hosts_racing_on_a_shared_line_keep_it_exactly_while_held();
    registering_while_applying_never_drops_a_record();
    a_repeated_change_answers_its_record_and_records_nothing();
    a_file_asked_for_new_content_is_rewritten_under_its_record();
    the_c_entry_point_records_like_the_json_one();
    a_link_left_where_a_file_was_is_removed_never_followed();
    an_appended_line_is_crlf_only_when_every_line_of_the_profile_is();
    return summary_with_links("changes_test");
}
