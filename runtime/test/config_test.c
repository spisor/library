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
#include "config.h"
#include "merge.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"
#include "temporary_entries.h"

#define BOM "\xEF\xBB\xBF"

typedef struct scratch_home {
    char path[1024];
    char config_directory[1100];
} scratch_home;

typedef struct answer {
    int32_t status;
    cJSON *tree;
    uint8_t *buffer;
} answer;

static void fresh_home(scratch_home *home, const char *label) {
    static int sequence = 0;
    snprintf(home->path, sizeof home->path, RUNTIME_SCRATCH_PATH "/config-%d-%d-%s", (int)CURRENT_PROCESS_ID, sequence++,
             label);
    snprintf(home->config_directory, sizeof home->config_directory, "%s/config", home->path);
    CHECK(spisor_make_directories(home->path) == 1);
}

static void write_text(const char *path, const char *text) {
    CHECK(spisor_replace_file(path, (const uint8_t *)text, strlen(text)) == 1);
}

static void write_settings_file(const scratch_home *home, const char *name, const char *text) {
    char path[1200];
    CHECK(spisor_make_directories(home->config_directory) == 1);
    snprintf(path, sizeof path, "%s/%s.json", home->config_directory, name);
    write_text(path, text);
}

static char *read_settings_file(const scratch_home *home, const char *name) {
    char path[1200];
    size_t length = 0;
    snprintf(path, sizeof path, "%s/%s.json", home->config_directory, name);
    return (char *)spisor_read_file(path, &length);
}

static cJSON *request_for(const scratch_home *home, const char *plugin, const char *config_json, const char *names_json) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "home", home->path);
    cJSON_AddStringToObject(request, "plugin", plugin);
    if (config_json != NULL) {
        cJSON_AddItemToObject(request, "config", cJSON_Parse(config_json));
    }
    if (names_json != NULL) {
        cJSON_AddItemToObject(request, "names", cJSON_Parse(names_json));
    }
    return request;
}

typedef int32_t (*export_function)(const uint8_t *, size_t, uint8_t **, size_t *);

static answer call_with(export_function function, cJSON *request) {
    answer result = {0, NULL, NULL};
    char *text = cJSON_PrintUnformatted(request);
    size_t out_len = 0;
    result.status = function((const uint8_t *)text, strlen(text), &result.buffer, &out_len);
    result.tree = result.buffer == NULL ? NULL : spisor_parse_json(result.buffer, out_len);
    free(text);
    cJSON_Delete(request);
    return result;
}

static void finish(answer *result) {
    cJSON_Delete(result->tree);
    spisor_release(result->buffer);
}

static answer get_key(const scratch_home *home, const char *plugin, const char *config_json, const char *key) {
    cJSON *request = request_for(home, plugin, config_json, NULL);
    cJSON_AddStringToObject(request, "key", key);
    return call_with(runtime_config_get, request);
}

static answer set_key(const scratch_home *home, const char *plugin, const char *config_json, const char *names_json,
                      const char *key, const char *value_json) {
    cJSON *request = request_for(home, plugin, config_json, names_json);
    cJSON_AddStringToObject(request, "key", key);
    cJSON_AddItemToObject(request, "value", cJSON_Parse(value_json));
    return call_with(runtime_config_set, request);
}

static void expect_member(const answer *result, const char *member, const char *expected_json, const char *label) {
    CHECK(result->status == SPISOR_STATUS_SUCCESS);
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(result->tree, member);
    if (expected_json == NULL) {
        if (item != NULL) {
            fprintf(stderr, "%s: expected %s absent\n", label, member);
            check_failures++;
        }
        return;
    }
    char *actual = item == NULL ? NULL : cJSON_PrintUnformatted(item);
    check_text(actual, expected_json, label);
    free(actual);
}

static void expect_refused(const answer *result, const char *label) {
    if (result->status != SPISOR_STATUS_FAILURE) {
        fprintf(stderr, "%s: expected a refusal, got status %d\n", label, (int)result->status);
        check_failures++;
        return;
    }
    CHECK(cJSON_GetObjectItemCaseSensitive(result->tree, "fix") != NULL);
}

static void expect_file(const scratch_home *home, const char *name, const char *expected, const char *label) {
    char *actual = read_settings_file(home, name);
    check_text(actual, expected, label);
    free(actual);
}

static void expect_value(const scratch_home *home, const char *plugin, const char *config_json, const char *key,
                         const char *expected_json, const char *label) {
    answer result = get_key(home, plugin, config_json, key);
    expect_member(&result, "value", expected_json, label);
    finish(&result);
}

static void name_rules(void) {
    cJSON *named = cJSON_Parse("{\"name\":\"antigravity\"}");
    cJSON *empty = cJSON_Parse("{\"name\":\"\"}");
    cJSON *escaping = cJSON_Parse("{\"name\":\"../x\"}");
    char *name = runtime_config_name("plugin-id", named);
    check_text(name, "antigravity", "declared name");
    free(name);
    name = runtime_config_name("plugin-id", empty);
    check_text(name, "plugin-id", "empty name falls back");
    free(name);
    name = runtime_config_name("plugin-id", NULL);
    check_text(name, "plugin-id", "absent config falls back");
    free(name);
    CHECK(runtime_config_name("plugin-id", escaping) == NULL);
    CHECK(runtime_config_name("../plugin", NULL) == NULL);
    cJSON_Delete(named);
    cJSON_Delete(empty);
    cJSON_Delete(escaping);

    scratch_home home;
    fresh_home(&home, "names");
    write_settings_file(&home, "antigravity", "{\"x\":1}");
    expect_value(&home, "p", "{\"name\":\"antigravity\"}", "x", "1", "declared name selects the file");
    answer refused = get_key(&home, "p", "{\"name\":\"../x\"}", "x");
    expect_refused(&refused, "escaping name");
    finish(&refused);
}

static void deep_merge_agrees(void) {
    scratch_home home;
    fresh_home(&home, "merge");
    const char *config = "{\"defaults\":{\"a\":{\"b\":1,\"c\":2}}}";
    write_settings_file(&home, "p", "{\"a\":{\"b\":5}}");
    expect_value(&home, "p", config, "a.c", "2", "get a.c");
    expect_value(&home, "p", config, "a", "{\"b\":5,\"c\":2}", "get a");
    answer all = call_with(runtime_config_all, request_for(&home, "p", config, NULL));
    expect_member(&all, "values", "{\"a\":{\"b\":5,\"c\":2}}", "all");
    finish(&all);
}

static void file_null_wins(void) {
    scratch_home home;
    fresh_home(&home, "null");
    write_settings_file(&home, "p", "{\"a\":null}");
    const char *config = "{\"defaults\":{\"a\":{\"b\":1}}}";
    expect_value(&home, "p", config, "a.b", NULL, "a.b under a null");
    expect_value(&home, "p", config, "a", "null", "a is null");
}

static void array_replaced_whole(void) {
    scratch_home home;
    fresh_home(&home, "array");
    write_settings_file(&home, "p", "{\"list\":[9]}");
    const char *config = "{\"defaults\":{\"list\":[1,2]}}";
    expect_value(&home, "p", config, "list.1", NULL, "list.1");
    expect_value(&home, "p", config, "list.0", "9", "list.0");
    expect_value(&home, "p", config, "list.00", NULL, "list.00");
    expect_value(&home, "p", config, "list.-0", NULL, "list.-0");
    expect_value(&home, "p", config, "list.length", NULL, "list.length");
}

static void absent_file_reads_defaults_and_writes_nothing(void) {
    scratch_home home;
    fresh_home(&home, "absent");
    answer all = call_with(runtime_config_all, request_for(&home, "p", "{\"defaults\":{\"x\":1}}", NULL));
    expect_member(&all, "values", "{\"x\":1}", "defaults");
    finish(&all);
    CHECK(spisor_file_exists(home.config_directory) == 0);
    answer bare = call_with(runtime_config_all, request_for(&home, "p", NULL, NULL));
    expect_member(&bare, "values", "{}", "no config");
    finish(&bare);
}

static void renamed_config_directory(void) {
    scratch_home home;
    char path[1200];
    fresh_home(&home, "renamed");
    answer result = set_key(&home, "p", NULL, "{\"config\":\"settings\"}", "k", "1");
    CHECK(result.status == SPISOR_STATUS_SUCCESS);
    finish(&result);
    snprintf(path, sizeof path, "%s/settings/p.json", home.path);
    CHECK(spisor_file_exists(path) != 0);
    CHECK(spisor_file_exists(home.config_directory) == 0);
}

static void set_previous_includes_default(void) {
    scratch_home home;
    fresh_home(&home, "previous");
    const char *config = "{\"defaults\":{\"x\":1}}";
    answer first = set_key(&home, "p", config, NULL, "x", "2");
    expect_member(&first, "previous", "1", "previous is the default");
    finish(&first);
    answer second = set_key(&home, "p", config, NULL, "x", "3");
    expect_member(&second, "previous", "2", "previous is the file value");
    finish(&second);
    answer fresh = set_key(&home, "p", config, NULL, "y", "1");
    expect_member(&fresh, "previous", NULL, "previous is absent");
    finish(&fresh);
}

static void set_twice_keeps_both(void) {
    scratch_home home;
    fresh_home(&home, "twice");
    answer first = set_key(&home, "p", NULL, NULL, "a", "1");
    finish(&first);
    answer second = set_key(&home, "p", NULL, NULL, "b", "2");
    finish(&second);
    expect_file(&home, "p", "{\n  \"a\": 1,\n  \"b\": 2\n}\n", "both keys");
    CHECK(count_temporary_entries(home.config_directory) == 0);
}

static void set_nests_new_path(void) {
    scratch_home home;
    fresh_home(&home, "nested");
    answer result = set_key(&home, "p", "{\"defaults\":{\"a\":{\"b\":1}}}", NULL, "a.c.d", "true");
    CHECK(result.status == SPISOR_STATUS_SUCCESS);
    finish(&result);
    expect_file(&home, "p", "{\n  \"a\": {\n    \"c\": {\n      \"d\": true\n    }\n  }\n}\n", "nested path from the file");
}

static void set_refuses_malformed_file(void) {
    scratch_home home;
    fresh_home(&home, "malformed");
    write_settings_file(&home, "p", "{\"a\":");
    answer result = set_key(&home, "p", NULL, NULL, "b", "1");
    expect_refused(&result, "malformed file");
    finish(&result);
    expect_file(&home, "p", "{\"a\":", "malformed file unchanged");
    write_settings_file(&home, "q", "[1]");
    answer array_root = set_key(&home, "q", NULL, NULL, "b", "1");
    expect_refused(&array_root, "array root");
    finish(&array_root);
    expect_file(&home, "q", "[1]", "array root unchanged");
}

static void set_refuses_non_object_on_path(void) {
    scratch_home home;
    fresh_home(&home, "crossing");
    const char *files[] = {"{\"a\":5}", "{\"a\":[1]}", "{\"a\":null}"};
    for (int index = 0; index < 3; index++) {
        write_settings_file(&home, "p", files[index]);
        answer result = set_key(&home, "p", NULL, NULL, "a.b", "1");
        expect_refused(&result, files[index]);
        finish(&result);
        expect_file(&home, "p", files[index], "file unchanged");
    }
}

static void set_refuses_empty_segment(void) {
    scratch_home home;
    fresh_home(&home, "empty");
    const char *keys[] = {"a..b", "", "a.", ".a"};
    for (int index = 0; index < 4; index++) {
        answer result = set_key(&home, "p", NULL, NULL, keys[index], "1");
        expect_refused(&result, keys[index]);
        finish(&result);
    }
    CHECK(spisor_file_exists(home.config_directory) == 0);
    expect_value(&home, "p", "{\"defaults\":{\"a\":1}}", "a.", NULL, "get with an empty segment is absent");
    expect_value(&home, "p", "{\"defaults\":{\"a\":1}}", "", NULL, "get with an empty key is absent");
}

static void set_refuses_missing_value(void) {
    scratch_home home;
    fresh_home(&home, "novalue");
    cJSON *request = request_for(&home, "p", NULL, NULL);
    cJSON_AddStringToObject(request, "key", "a");
    answer result = call_with(runtime_config_set, request);
    expect_refused(&result, "missing value");
    finish(&result);
    CHECK(spisor_file_exists(home.config_directory) == 0);
}

static void bom_file(void) {
    scratch_home home;
    fresh_home(&home, "bom");
    write_settings_file(&home, "p", BOM "{\"a\":1}");
    expect_value(&home, "p", "{\"defaults\":{\"a\":2}}", "a", "2", "a BOM file reads as absent content");
    answer result = set_key(&home, "p", NULL, NULL, "b", "1");
    expect_refused(&result, "BOM file");
    finish(&result);
    expect_file(&home, "p", BOM "{\"a\":1}", "BOM file unchanged");
}

static void comment_file(void) {
    scratch_home home;
    fresh_home(&home, "comment");
    write_settings_file(&home, "p", "// note\n{\"a\":1}");
    expect_value(&home, "p", NULL, "a", "1", "comment line stripped");
    write_settings_file(&home, "p", BOM "// note\n{\"a\":3}");
    expect_value(&home, "p", NULL, "a", "3", "BOM before a comment");
}

static void hand_edit_seen(void) {
    scratch_home home;
    fresh_home(&home, "edit");
    write_settings_file(&home, "p", "{\"a\":1}");
    expect_value(&home, "p", NULL, "a", "1", "before the edit");
    write_settings_file(&home, "p", "{\"a\":2}");
    expect_value(&home, "p", NULL, "a", "2", "after the edit");
}

static void merge_and_walk_units(void) {
    cJSON *defaults = cJSON_Parse("{\"a\":{\"b\":1},\"n\":[1]}");
    cJSON *file = cJSON_Parse("{\"a\":5,\"z\":true}");
    cJSON *merged = runtime_merge(defaults, file);
    char *text = merged == NULL ? NULL : cJSON_PrintUnformatted(merged);
    check_text(text, "{\"a\":5,\"n\":[1],\"z\":true}", "scalar over object");
    free(text);
    CHECK(runtime_walk(merged, "n.0") != NULL);
    CHECK(runtime_walk(merged, "n.1") == NULL);
    CHECK(runtime_walk(merged, "a.b") == NULL);
    cJSON_Delete(merged);
    cJSON_Delete(defaults);
    cJSON_Delete(file);
}

static void refuses_missing_arguments(void) {
    const char *request = "{\"home\":\"x\"}";
    uint8_t *out = NULL;
    size_t out_len = 0;
    CHECK(runtime_config_all((const uint8_t *)request, strlen(request), &out, &out_len) == SPISOR_STATUS_FAILURE);
    spisor_release(out);
}

int main(void) {
    name_rules();
    deep_merge_agrees();
    file_null_wins();
    array_replaced_whole();
    absent_file_reads_defaults_and_writes_nothing();
    renamed_config_directory();
    set_previous_includes_default();
    set_twice_keeps_both();
    set_nests_new_path();
    set_refuses_malformed_file();
    set_refuses_non_object_on_path();
    set_refuses_empty_segment();
    set_refuses_missing_value();
    bom_file();
    comment_file();
    hand_edit_seen();
    merge_and_walk_units();
    refuses_missing_arguments();
    return check_summary("config_test");
}
