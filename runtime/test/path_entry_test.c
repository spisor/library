#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#include <wchar.h>
#include <windows.h>
#define CURRENT_PROCESS_ID _getpid()
#else
#include <unistd.h>
#define CURRENT_PROCESS_ID getpid()
#endif

#include "changes.h"
#include "check.h"
#include "directory_tree.h"
#include "registry.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"

#ifdef _WIN32
#include "path_entry_windows_hooks.h"
#endif

typedef int32_t (*changes_export)(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

typedef struct scratch {
    char directory[1024];
    char home[1100];
    char state[1100];
    char shims[1200];
} scratch;

typedef struct answer {
    int32_t status;
    cJSON *tree;
} answer;

static void fresh_scratch(scratch *place, const char *label) {
    static int sequence = 0;
    snprintf(place->directory, sizeof place->directory, RUNTIME_SCRATCH_PATH "/path-entry-%d-%d-%s",
             (int)CURRENT_PROCESS_ID, sequence++, label);
    snprintf(place->home, sizeof place->home, "%s/home", place->directory);
    snprintf(place->state, sizeof place->state, "%s/state", place->directory);
    snprintf(place->shims, sizeof place->shims, "%s/bin", place->state);
    CHECK(spisor_make_directories(place->home) == 1);
    CHECK(spisor_make_directories(place->shims) == 1);
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

static cJSON *places_for(const scratch *place) {
    cJSON *places = cJSON_CreateObject();
    cJSON_AddStringToObject(places, "state", place->state);
    return places;
}

static void register_host(const scratch *place, const char *id) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddItemToObject(request, "places", places_for(place));
    cJSON *registration = cJSON_AddObjectToObject(request, "registration");
    cJSON_AddStringToObject(registration, "id", id);
    cJSON_AddStringToObject(registration, "kind", "host");
    cJSON_AddStringToObject(registration, "version", "1.0.0");
    cJSON *provenance = cJSON_AddObjectToObject(registration, "provenance");
    cJSON_AddStringToObject(provenance, "method", "npm");
    cJSON_AddStringToObject(provenance, "version", "1.0.0");
    cJSON_AddStringToObject(provenance, "location", place->directory);
    cJSON_AddBoolToObject(registration, "requiresCommand", 0);
    answer result = call(runtime_registry_register, request);
    CHECK(result.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(result.tree);
}

static void register_hosts(const scratch *place) {
    register_host(place, "alpha");
    register_host(place, "beta");
}

static cJSON *host_request(const scratch *place, const char *host) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddItemToObject(request, "places", places_for(place));
    cJSON_AddStringToObject(request, "host", host);
    return request;
}

static cJSON *path_entry_change(const scratch *place, const char *directory) {
    cJSON *change = cJSON_CreateObject();
    cJSON_AddStringToObject(change, "kind", "path-entry");
    cJSON_AddStringToObject(change, "directory", directory);
    cJSON_AddStringToObject(change, "home", place->home);
    return change;
}

static answer apply_entry(const scratch *place, const char *host, const char *directory) {
    cJSON *request = host_request(place, host);
    cJSON_AddItemToObject(request, "change", path_entry_change(place, directory));
    return call(runtime_changes_apply_json, request);
}

static long applied_sequence(const scratch *place, const char *host, const char *directory) {
    answer result = apply_entry(place, host, directory);
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

static int apply_unneeded(const scratch *place, const char *host, const char *directory) {
    answer result = apply_entry(place, host, directory);
    int unneeded = result.status == SPISOR_STATUS_SUCCESS &&
                   cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(result.tree, "recorded"));
    cJSON_Delete(result.tree);
    return unneeded;
}

static int answered_existing(const scratch *place, const char *host, const char *directory, long expected) {
    answer result = apply_entry(place, host, directory);
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

static int apply_refused(const scratch *place, const char *host, const char *directory) {
    answer result = apply_entry(place, host, directory);
    int refused = result.status == SPISOR_STATUS_FAILURE;
    cJSON_Delete(result.tree);
    return refused;
}

static int reverse_counts(const scratch *place, const char *host, int expected_reversed, int expected_drifted) {
    answer result = call(runtime_changes_reverse_json, host_request(place, host));
    const cJSON *reversed = cJSON_GetObjectItemCaseSensitive(result.tree, "reversed");
    const cJSON *drifted = cJSON_GetObjectItemCaseSensitive(result.tree, "drifted");
    int matches = result.status == SPISOR_STATUS_SUCCESS && cJSON_IsNumber(reversed) &&
                  reversed->valueint == expected_reversed && cJSON_IsArray(drifted) &&
                  cJSON_GetArraySize(drifted) == expected_drifted;
    if (!matches) {
        char *text = result.tree == NULL ? NULL : cJSON_PrintUnformatted(result.tree);
        fprintf(stderr, "reverse of %s: expected %d reversed and %d drifted, got status %d %s\n", host,
                expected_reversed, expected_drifted, (int)result.status, text == NULL ? "" : text);
        free(text);
    }
    cJSON_Delete(result.tree);
    return matches;
}

static int change_count(const scratch *place, const char *host) {
    cJSON *registration = NULL;
    CHECK(runtime_registry_load(place->state, host, &registration) == RUNTIME_REGISTRY_OK);
    const cJSON *changes = cJSON_GetObjectItemCaseSensitive(registration, "changes");
    int count = cJSON_IsArray(changes) ? cJSON_GetArraySize(changes) : 0;
    cJSON_Delete(registration);
    return count;
}

static void refusals_record_nothing(const scratch *place) {
    CHECK(apply_refused(place, "alpha", "relative/bin"));
    char directory[1400];
#ifdef _WIN32
    snprintf(directory, sizeof directory, "%s/a;b", place->state);
    CHECK(apply_refused(place, "alpha", directory));
    snprintf(directory, sizeof directory, "%s/%%TEMP%%", place->state);
    CHECK(apply_refused(place, "alpha", directory));
#else
    snprintf(directory, sizeof directory, "%s/b$in", place->state);
    CHECK(apply_refused(place, "alpha", directory));
    snprintf(directory, sizeof directory, "%s/a:b", place->state);
    CHECK(apply_refused(place, "alpha", directory));
    snprintf(directory, sizeof directory, "%s/a\"b", place->state);
    CHECK(apply_refused(place, "alpha", directory));
#endif
    CHECK(change_count(place, "alpha") == 0);
}

#ifdef _WIN32

#define ENVIRONMENT_KEY L"Environment"
#define READ_FLAGS (RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND)

typedef struct registry_text {
    wchar_t text[8192];
    DWORD type;
    int present;
} registry_text;

static void delete_test_value(void) {
    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, ENVIRONMENT_KEY, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, RUNTIME_PATH_ENTRY_TEST_VALUE);
        RegCloseKey(key);
    }
}

static void set_test_value(DWORD type, const wchar_t *text) {
    DWORD size = (DWORD)((wcslen(text) + 1) * sizeof(wchar_t));
    CHECK(RegSetKeyValueW(HKEY_CURRENT_USER, ENVIRONMENT_KEY, RUNTIME_PATH_ENTRY_TEST_VALUE, type, text, size) ==
          ERROR_SUCCESS);
}

static void read_value(const wchar_t *name, registry_text *value) {
    DWORD size = sizeof value->text;
    value->type = 0;
    value->text[0] = L'\0';
    LSTATUS status =
        RegGetValueW(HKEY_CURRENT_USER, ENVIRONMENT_KEY, name, READ_FLAGS, &value->type, value->text, &size);
    value->present = status == ERROR_SUCCESS;
    CHECK(status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND);
}

/** Whether the test value is stored as expected, unexpanded; a NULL expected means the value is absent. */
static int test_value_is(DWORD expected_type, const wchar_t *expected) {
    registry_text value;
    read_value(RUNTIME_PATH_ENTRY_TEST_VALUE, &value);
    int same = expected == NULL ? !value.present
                                : value.present && value.type == expected_type && wcscmp(value.text, expected) == 0;
    if (!same) {
        fwprintf(stderr, L"test value: expected type %lu \"%ls\", got present %d type %lu \"%ls\"\n",
                 (unsigned long)expected_type, expected == NULL ? L"(absent)" : expected, value.present,
                 (unsigned long)value.type, value.text);
        check_failures++;
    }
    return same;
}

/** The directory as runtime stores it: backslashes, as wide text. */
static void stored_form(const char *directory, wchar_t *wide, size_t count) {
    char backslashed[1400];
    snprintf(backslashed, sizeof backslashed, "%s", directory);
    for (char *character = backslashed; *character != '\0'; character++) {
        *character = *character == '/' ? '\\' : *character;
    }
    CHECK(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, backslashed, -1, wide, (int)count) > 0);
}

/** The same directory spelled another way: upper case, forward slashes and a trailing slash. */
static void other_spelling(const char *directory, char *variant, size_t size) {
    snprintf(variant, size, "%s/", directory);
    for (char *character = variant; *character != '\0'; character++) {
        *character = (char)toupper((unsigned char)*character);
    }
}

static void two_hosts_keep_the_directory_until_the_last_is_reversed(void) {
    scratch place;
    fresh_scratch(&place, "two-hosts");
    register_hosts(&place);
    const wchar_t *before = L"%USERPROFILE%\\spisor-test-tools;C:\\Other";
    set_test_value(REG_EXPAND_SZ, before);
    wchar_t entry[1400];
    stored_form(place.shims, entry, 1400);
    wchar_t with_entry[4096];
    swprintf(with_entry, 4096, L"%ls;%ls", before, entry);
    char variant[1400];
    other_spelling(place.shims, variant, sizeof variant);

    CHECK(applied_sequence(&place, "alpha", place.shims) == 1);
    CHECK(test_value_is(REG_EXPAND_SZ, with_entry));
    CHECK(applied_sequence(&place, "beta", variant) == 1);
    CHECK(test_value_is(REG_EXPAND_SZ, with_entry));
    CHECK(reverse_counts(&place, "alpha", 1, 0));
    CHECK(test_value_is(REG_EXPAND_SZ, with_entry));
    CHECK(reverse_counts(&place, "beta", 1, 0));
    CHECK(test_value_is(REG_EXPAND_SZ, before));
    delete_test_value();
}

static void an_entry_the_user_had_is_never_removed(void) {
    scratch place;
    fresh_scratch(&place, "user-had");
    register_hosts(&place);
    wchar_t entry[1400];
    stored_form(place.shims, entry, 1400);
    wchar_t users[4096];
    swprintf(users, 4096, L"C:\\Other;%ls", _wcsupr(entry));
    set_test_value(REG_SZ, users);
    CHECK(apply_unneeded(&place, "alpha", place.shims));
    CHECK(change_count(&place, "alpha") == 0);
    CHECK(reverse_counts(&place, "alpha", 0, 0));
    CHECK(test_value_is(REG_SZ, users));
    delete_test_value();
}

static void an_absent_value_is_created_expandable_and_removed_again(void) {
    scratch place;
    fresh_scratch(&place, "absent");
    register_hosts(&place);
    delete_test_value();
    wchar_t entry[1400];
    stored_form(place.shims, entry, 1400);
    CHECK(applied_sequence(&place, "alpha", place.shims) == 1);
    CHECK(test_value_is(REG_EXPAND_SZ, entry));
    CHECK(reverse_counts(&place, "alpha", 1, 0));
    CHECK(test_value_is(REG_EXPAND_SZ, NULL));
}

static void only_the_exact_entry_leaves_when_the_user_appended_after_it(void) {
    scratch place;
    fresh_scratch(&place, "appended");
    register_hosts(&place);
    set_test_value(REG_EXPAND_SZ, L"C:\\Other;");
    wchar_t entry[1400];
    stored_form(place.shims, entry, 1400);
    CHECK(applied_sequence(&place, "alpha", place.shims) == 1);
    wchar_t appended[4096];
    swprintf(appended, 4096, L"C:\\Other;%ls", entry);
    CHECK(test_value_is(REG_EXPAND_SZ, appended));
    wchar_t users[4096];
    swprintf(users, 4096, L"%ls;C:\\Later", appended);
    set_test_value(REG_EXPAND_SZ, users);
    CHECK(reverse_counts(&place, "alpha", 1, 0));
    CHECK(test_value_is(REG_EXPAND_SZ, L"C:\\Other;C:\\Later"));
    delete_test_value();
}

static void a_repeated_apply_records_nothing(void) {
    scratch place;
    fresh_scratch(&place, "repeat");
    register_hosts(&place);
    const wchar_t *before = L"C:\\Other";
    set_test_value(REG_EXPAND_SZ, before);
    wchar_t entry[1400];
    stored_form(place.shims, entry, 1400);
    wchar_t with_entry[4096];
    swprintf(with_entry, 4096, L"%ls;%ls", before, entry);
    CHECK(applied_sequence(&place, "alpha", place.shims) == 1);
    CHECK(answered_existing(&place, "alpha", place.shims, 1));
    CHECK(answered_existing(&place, "alpha", place.shims, 1));
    CHECK(change_count(&place, "alpha") == 1);
    CHECK(test_value_is(REG_EXPAND_SZ, with_entry));
    CHECK(reverse_counts(&place, "alpha", 1, 0));
    CHECK(test_value_is(REG_EXPAND_SZ, before));
    delete_test_value();
}

static void windows_refusals_record_nothing(void) {
    scratch place;
    fresh_scratch(&place, "refusals");
    register_hosts(&place);
    refusals_record_nothing(&place);
    CHECK(test_value_is(REG_EXPAND_SZ, NULL));
}

int main(void) {
    registry_text real_before;
    read_value(L"Path", &real_before);
    delete_test_value();
    two_hosts_keep_the_directory_until_the_last_is_reversed();
    an_entry_the_user_had_is_never_removed();
    an_absent_value_is_created_expandable_and_removed_again();
    only_the_exact_entry_leaves_when_the_user_appended_after_it();
    a_repeated_apply_records_nothing();
    windows_refusals_record_nothing();
    delete_test_value();
    registry_text real_after;
    read_value(L"Path", &real_after);
    CHECK(real_before.present == real_after.present && real_before.type == real_after.type &&
          wcscmp(real_before.text, real_after.text) == 0);
    return check_summary("path_entry_test");
}

#else

static void in_home(const scratch *place, const char *relative, char *path, size_t size) {
    snprintf(path, size, "%s/%s", place->home, relative);
}

static void write_home_file(const scratch *place, const char *relative, const char *text) {
    char path[1400];
    in_home(place, relative, path, sizeof path);
    CHECK(spisor_replace_file(path, (const uint8_t *)text, strlen(text)) == 1);
}

static int home_file_is(const scratch *place, const char *relative, const char *expected) {
    char path[1400];
    in_home(place, relative, path, sizeof path);
    size_t length = 0;
    char *actual = (char *)spisor_read_file(path, &length);
    int same = check_text(actual, expected, relative);
    free(actual);
    return same;
}

static int home_entry_exists(const scratch *place, const char *relative) {
    char path[1400];
    in_home(place, relative, path, sizeof path);
    return runtime_probe_entry(path) != RUNTIME_ENTRY_MISSING;
}

static void export_line(const char *directory, char *line, size_t size) {
    snprintf(line, size, "export PATH=\"%s:$PATH\"", directory);
}

static void with_line(const char *before, const char *directory, char *after, size_t size) {
    char line[1500];
    export_line(directory, line, sizeof line);
    snprintf(after, size, "%s%s%s\n", before, before[0] != '\0' && before[strlen(before) - 1] != '\n' ? "\n" : "",
             line);
}

static int is_kind(const cJSON *entry, const char *kind) {
    const char *actual = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "kind"));
    return actual != NULL && strcmp(actual, kind) == 0;
}

static int children_point_at_their_parent(const scratch *place, const char *host, int expected_children) {
    cJSON *registration = NULL;
    CHECK(runtime_registry_load(place->state, host, &registration) == RUNTIME_REGISTRY_OK);
    const cJSON *changes = cJSON_GetObjectItemCaseSensitive(registration, "changes");
    const cJSON *parent = cJSON_GetArrayItem(changes, 0);
    int matches = cJSON_GetArraySize(changes) == expected_children + 1 && is_kind(parent, "path-entry");
    for (int index = 1; matches && index <= expected_children; index++) {
        const cJSON *child = cJSON_GetArrayItem(changes, index);
        const cJSON *pointer = cJSON_GetObjectItemCaseSensitive(child, "parent");
        matches = is_kind(child, "profile-line") && cJSON_IsNumber(pointer) &&
                  pointer->valuedouble == cJSON_GetObjectItemCaseSensitive(parent, "sequence")->valuedouble;
    }
    cJSON_Delete(registration);
    return matches;
}

static void two_hosts_keep_the_directory_until_the_last_is_reversed(void) {
    scratch place;
    fresh_scratch(&place, "two-hosts");
    register_hosts(&place);
    const char *profile_before = "# profile\n";
    const char *bashrc_before = "alias ll='ls -l'";
    write_home_file(&place, ".profile", profile_before);
    write_home_file(&place, ".bashrc", bashrc_before);
    char profile_after[2048];
    char bashrc_after[2048];
    with_line(profile_before, place.shims, profile_after, sizeof profile_after);
    with_line(bashrc_before, place.shims, bashrc_after, sizeof bashrc_after);
    char variant[1400];
    snprintf(variant, sizeof variant, "%s/", place.shims);

    CHECK(applied_sequence(&place, "alpha", place.shims) == 1);
    CHECK(children_point_at_their_parent(&place, "alpha", 2));
    CHECK(home_file_is(&place, ".profile", profile_after));
    CHECK(home_file_is(&place, ".bashrc", bashrc_after));
    CHECK(!home_entry_exists(&place, ".zshrc"));
    CHECK(applied_sequence(&place, "beta", variant) == 1);
    CHECK(home_file_is(&place, ".profile", profile_after));
    CHECK(home_file_is(&place, ".bashrc", bashrc_after));
    CHECK(reverse_counts(&place, "alpha", 3, 0));
    CHECK(home_file_is(&place, ".profile", profile_after));
    CHECK(home_file_is(&place, ".bashrc", bashrc_after));
    CHECK(reverse_counts(&place, "beta", 3, 0));
    CHECK(home_file_is(&place, ".profile", profile_before));
    CHECK(home_file_is(&place, ".bashrc", bashrc_before));
    CHECK(!home_entry_exists(&place, ".zshrc"));
}

static void a_line_the_user_had_is_never_removed(void) {
    scratch place;
    fresh_scratch(&place, "user-had");
    register_hosts(&place);
    char users[2048];
    with_line("", place.shims, users, sizeof users);
    write_home_file(&place, ".bashrc", users);
    write_home_file(&place, ".profile", "# profile\n");
    CHECK(applied_sequence(&place, "alpha", place.shims) == 1);
    CHECK(children_point_at_their_parent(&place, "alpha", 1));
    CHECK(reverse_counts(&place, "alpha", 2, 0));
    CHECK(home_file_is(&place, ".bashrc", users));
    CHECK(home_file_is(&place, ".profile", "# profile\n"));

    write_home_file(&place, ".profile", users);
    CHECK(apply_unneeded(&place, "beta", place.shims));
    CHECK(change_count(&place, "beta") == 0);
    CHECK(home_file_is(&place, ".profile", users));
}

static void a_repeated_apply_adds_only_a_profile_new_since(void) {
    scratch place;
    fresh_scratch(&place, "repeat");
    register_hosts(&place);
    write_home_file(&place, ".profile", "# profile\n");
    char profile_after[2048];
    with_line("# profile\n", place.shims, profile_after, sizeof profile_after);
    CHECK(applied_sequence(&place, "alpha", place.shims) == 1);
    CHECK(answered_existing(&place, "alpha", place.shims, 1));
    CHECK(change_count(&place, "alpha") == 2);

    write_home_file(&place, ".zshrc", "# zsh\n");
    char zshrc_after[2048];
    with_line("# zsh\n", place.shims, zshrc_after, sizeof zshrc_after);
    CHECK(applied_sequence(&place, "alpha", place.shims) == 1);
    CHECK(change_count(&place, "alpha") == 3);
    CHECK(answered_existing(&place, "alpha", place.shims, 1));
    CHECK(change_count(&place, "alpha") == 3);
    CHECK(home_file_is(&place, ".profile", profile_after));
    CHECK(home_file_is(&place, ".zshrc", zshrc_after));
    CHECK(reverse_counts(&place, "alpha", 3, 0));
    CHECK(home_file_is(&place, ".profile", "# profile\n"));
    CHECK(home_file_is(&place, ".zshrc", "# zsh\n"));
}

static void no_profile_means_refused_and_none_created(void) {
    scratch place;
    fresh_scratch(&place, "no-profile");
    register_hosts(&place);
    CHECK(apply_refused(&place, "alpha", place.shims));
    CHECK(change_count(&place, "alpha") == 0);
    CHECK(!home_entry_exists(&place, ".profile"));
    CHECK(!home_entry_exists(&place, ".bashrc"));
    CHECK(!home_entry_exists(&place, ".zshrc"));
}

static void posix_refusals_record_nothing(void) {
    scratch place;
    fresh_scratch(&place, "refusals");
    register_hosts(&place);
    write_home_file(&place, ".profile", "# profile\n");
    refusals_record_nothing(&place);
    CHECK(home_file_is(&place, ".profile", "# profile\n"));
}

int main(void) {
    two_hosts_keep_the_directory_until_the_last_is_reversed();
    a_line_the_user_had_is_never_removed();
    a_repeated_apply_adds_only_a_profile_new_since();
    no_profile_means_refused_and_none_created();
    posix_refusals_record_nothing();
    return check_summary("path_entry_test");
}

#endif
