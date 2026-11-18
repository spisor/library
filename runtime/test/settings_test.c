#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#define CURRENT_PROCESS_ID _getpid()
#else
#include <unistd.h>
#define CURRENT_PROCESS_ID getpid()
#endif

#include "check.h"
#include "spisor/allocation.h"
#include "config_fixture.h"
#include "scratch_directory.h"
#include "config.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"
#include "links.h"
#include "temporary_entries.h"

typedef struct scratch_home {
    char path[1024];
    char application_file[1100];
    char portable[1100];
    char portable_file[1200];
} scratch_home;

typedef config_answer answer;

static void fresh_home(scratch_home *home, const char *label) {
    static int sequence = 0;
    snprintf(home->path, sizeof home->path, RUNTIME_SCRATCH_PATH "/settings-%d-%d-%s", (int)CURRENT_PROCESS_ID,
             sequence++, label);
    snprintf(home->application_file, sizeof home->application_file, "%s/config/settings.json", home->path);
    snprintf(home->portable, sizeof home->portable, "%s/portable", home->path);
    snprintf(home->portable_file, sizeof home->portable_file, "%s/settings.json", home->portable);
    start_empty_directory(home->path);
}

static void write_text(const char *path, const char *text) {
    CHECK(spisor_replace_file(path, (const uint8_t *)text, strlen(text)) == SPISOR_OK);
}

static void write_application_file(const scratch_home *home, const char *text) {
    char directory[1200];
    snprintf(directory, sizeof directory, "%s/config", home->path);
    CHECK(spisor_make_directories(directory) == SPISOR_OK);
    write_text(home->application_file, text);
}

static void write_portable_file(const scratch_home *home, const char *text) {
    CHECK(spisor_make_directories(home->portable) == SPISOR_OK);
    write_text(home->portable_file, text);
}

static void expect_content(const char *path, const char *expected, const char *label) {
    size_t length = 0;
    char *actual = (char *)spisor_read_file(path, &length);
    check_text(actual, expected, label);
    spisor_memory_release(actual);
}

static cJSON *request_for(const scratch_home *home, int with_portable) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "home", home->path);
    if (with_portable) {
        cJSON_AddStringToObject(request, "portable", home->portable);
    }
    return request;
}

static answer call_with(config_operation operation, cJSON *request) {
    return config_call(operation, request);
}

static void finish(answer *result) {
    cJSON_Delete(result->tree);
}

static answer settings_set(const scratch_home *home, const char *layer, const char *key, const char *value_json) {
    cJSON *request = request_for(home, 1);
    cJSON_AddStringToObject(request, "key", key);
    cJSON_AddItemToObject(request, "value", cJSON_Parse(value_json));
    if (layer != NULL) {
        cJSON_AddStringToObject(request, "layer", layer);
    }
    return call_with(SETTINGS_SET, request);
}

static void expect_values(const scratch_home *home, const char *expected_json, const char *label) {
    answer result = call_with(SETTINGS_ALL, request_for(home, 1));
    CHECK(ANSWERED(result));
    const cJSON *values = cJSON_GetObjectItemCaseSensitive(result.tree, "values");
    char *actual = values == NULL ? NULL : cJSON_PrintUnformatted(values);
    check_text(actual, expected_json, label);
    spisor_memory_release(actual);
    finish(&result);
}

static void expect_refused_naming(const answer *result, const char *path, const char *label) {
    const cJSON *fix = cJSON_GetObjectItemCaseSensitive(result->tree, "fix");
    if (!REFUSED_CALL(*result) || !cJSON_IsString(fix) || strstr(fix->valuestring, path) == NULL) {
        fprintf(stderr, "%s: expected a refusal whose fix names %s\n", label, path);
        check_failures++;
    }
}

static void all_merges_portable_under_application(void) {
    scratch_home home;
    fresh_home(&home, "merge");
    write_portable_file(&home, "{\"ui\":{\"theme\":\"dark\",\"size\":1},\"only\":true}");
    write_application_file(&home, "{\"ui\":{\"size\":2}}");
    expect_values(&home, "{\"ui\":{\"theme\":\"dark\",\"size\":2},\"only\":true}", "application over portable");
    answer without_portable = call_with(SETTINGS_ALL, request_for(&home, 0));
    CHECK(ANSWERED(without_portable));
    finish(&without_portable);
}

static void all_on_an_empty_home_is_empty_and_creates_nothing(void) {
    scratch_home home;
    fresh_home(&home, "empty");
    expect_values(&home, "{}", "no files");
    CHECK(spisor_probe_file(home.application_file) != SPISOR_FILE_PRESENT);
}

static void set_without_a_layer_writes_the_application_file(void) {
    scratch_home home;
    fresh_home(&home, "application");
    answer result = settings_set(&home, NULL, "a", "1");
    CHECK(ANSWERED(result));
    CHECK(cJSON_GetObjectItemCaseSensitive(result.tree, "previous") == NULL);
    finish(&result);
    expect_content(home.application_file, "{\n  \"a\": 1\n}\n", "the application file");
    CHECK(spisor_probe_file(home.portable_file) != SPISOR_FILE_PRESENT);
}

static void set_in_the_portable_layer_writes_only_that_file(void) {
    scratch_home home;
    fresh_home(&home, "portable");
    write_application_file(&home, "{\"kept\":1}");
    answer result = settings_set(&home, "portable", "b", "2");
    CHECK(ANSWERED(result));
    finish(&result);
    expect_content(home.portable_file, "{\n  \"b\": 2\n}\n", "the portable file");
    expect_content(home.application_file, "{\"kept\":1}", "the application file untouched");
}

static void set_reports_the_merged_previous(void) {
    scratch_home home;
    fresh_home(&home, "previous");
    write_portable_file(&home, "{\"x\":7}");
    answer result = settings_set(&home, NULL, "x", "8");
    const cJSON *previous = cJSON_GetObjectItemCaseSensitive(result.tree, "previous");
    CHECK(ANSWERED(result) && cJSON_IsNumber(previous) && previous->valuedouble == 7);
    finish(&result);
}

static void set_nests_a_dotted_key(void) {
    scratch_home home;
    fresh_home(&home, "dotted");
    answer first = settings_set(&home, NULL, "plugins.demo.enabled", "true");
    CHECK(ANSWERED(first));
    finish(&first);
    answer second = settings_set(&home, NULL, "plugins.demo.size", "3");
    CHECK(ANSWERED(second));
    finish(&second);
    expect_values(&home, "{\"plugins\":{\"demo\":{\"enabled\":true,\"size\":3}}}", "nested object");
}

static void set_refuses_a_bad_key_a_missing_value_and_a_bad_layer(void) {
    scratch_home home;
    fresh_home(&home, "refusals");
    answer empty_segment = settings_set(&home, NULL, "a..b", "1");
    CHECK(REFUSED_CALL(empty_segment));
    finish(&empty_segment);
    answer unknown_layer = settings_set(&home, "machine", "a", "1");
    CHECK(REFUSED_CALL(unknown_layer));
    finish(&unknown_layer);
    cJSON *no_value = request_for(&home, 1);
    cJSON_AddStringToObject(no_value, "key", "a");
    answer missing = call_with(SETTINGS_SET, no_value);
    CHECK(REFUSED_CALL(missing));
    finish(&missing);
    answer no_home = call_with(SETTINGS_ALL, cJSON_CreateObject());
    CHECK(REFUSED_CALL(no_home));
    finish(&no_home);
    CHECK(spisor_probe_file(home.application_file) != SPISOR_FILE_PRESENT);
}

static void set_in_portable_without_portable_is_refused(void) {
    scratch_home home;
    fresh_home(&home, "no-portable");
    cJSON *request = request_for(&home, 0);
    cJSON_AddStringToObject(request, "key", "a");
    cJSON_AddNumberToObject(request, "value", 1);
    cJSON_AddStringToObject(request, "layer", "portable");
    answer result = call_with(SETTINGS_SET, request);
    CHECK(REFUSED_CALL(result));
    finish(&result);
    CHECK(spisor_probe_file(home.application_file) != SPISOR_FILE_PRESENT);
}

static void an_unreadable_portable_file_is_refused_and_kept(void) {
    scratch_home home;
    fresh_home(&home, "malformed");
    write_portable_file(&home, "{\"a\":");
    answer refused = settings_set(&home, "portable", "b", "1");
    expect_refused_naming(&refused, "settings.json", "malformed portable file");
    finish(&refused);
    expect_content(home.portable_file, "{\"a\":", "the portable file unchanged");
    answer application = settings_set(&home, NULL, "b", "1");
    CHECK(ANSWERED(application));
    finish(&application);
}

static void set_writes_through_a_symlinked_settings_file(void) {
    scratch_home home;
    char dotfiles[1200];
    char target[1300];
    fresh_home(&home, "link");
    snprintf(dotfiles, sizeof dotfiles, "%s/dotfiles", home.path);
    snprintf(target, sizeof target, "%s/settings.json", dotfiles);
    CHECK(spisor_make_directories(dotfiles) == SPISOR_OK);
    write_text(target, "{\"kept\":true}");
    write_portable_file(&home, "{}");
    if (!create_link_or_skip(home.portable_file, target)) {
        return;
    }
    answer result = settings_set(&home, "portable", "added", "1");
    CHECK(ANSWERED(result));
    finish(&result);
    CHECK(is_link(home.portable_file));
    expect_content(target, "{\n  \"kept\": true,\n  \"added\": 1\n}\n", "the link's target changed");
    CHECK(count_temporary_entries(dotfiles) == 0);
}

static void a_file_holding_a_number_no_value_carries_reads_as_unreadable(void) {
    scratch_home home;
    fresh_home(&home, "non-finite");
    write_application_file(&home, "{\"a\":1e999,\"b\":1}");
    answer set = settings_set(&home, NULL, "a", "5");
    CHECK(set.status == SPISOR_REFUSED);
    CHECK(strstr(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(set.tree, "message")), "readable") != NULL);
    finish(&set);
    expect_content(home.application_file, "{\"a\":1e999,\"b\":1}", "the refused set left the file byte-identical");
    expect_values(&home, "{}", "the file contributes nothing");
}

static void a_file_nested_past_what_json_parses_contributes_nothing(void) {
    scratch_home home;
    char text[2 * 1001 + 16];
    fresh_home(&home, "nested");
    memcpy(text, "{\"a\":", 5);
    memset(text + 5, '[', 1001);
    memset(text + 5 + 1001, ']', 1001);
    memcpy(text + 5 + 2002, "}", 2);
    write_portable_file(&home, "{\"b\":1}");
    write_application_file(&home, text);
    expect_values(&home, "{\"b\":1}", "a file nested past cJSON's limit contributes nothing");
}

int main(void) {
    a_file_nested_past_what_json_parses_contributes_nothing();
    a_file_holding_a_number_no_value_carries_reads_as_unreadable();
    all_merges_portable_under_application();
    all_on_an_empty_home_is_empty_and_creates_nothing();
    set_without_a_layer_writes_the_application_file();
    set_in_the_portable_layer_writes_only_that_file();
    set_reports_the_merged_previous();
    set_nests_a_dotted_key();
    set_refuses_a_bad_key_a_missing_value_and_a_bad_layer();
    set_in_portable_without_portable_is_refused();
    an_unreadable_portable_file_is_refused_and_kept();
    set_writes_through_a_symlinked_settings_file();
    return summary_with_links("settings_test");
}
