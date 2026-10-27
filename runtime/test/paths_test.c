#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "paths.h"
#include "segment.h"
#include "spisor/json_kind.h"
#include "spisor/path.h"

#if defined(_WIN32)
#define TEST_HOME "C:\\h"
#define TEST_HOME_JSON "C:\\\\h"
#define TEST_WINDOWS 1
#else
#define TEST_HOME "/h"
#define TEST_HOME_JSON "/h"
#define TEST_WINDOWS 0
#endif

static char *joined(const char *name) {
    const char *segments[2] = {TEST_HOME, name};
    return spisor_path_join(TEST_WINDOWS, segments, 2);
}

static void expect_joined(const char *actual, const char *name, const char *label) {
    char *expected = joined(name);
    CHECK(expected != NULL);
    if (expected != NULL) {
        check_text(actual, expected, label);
    }
    free(expected);
}

static void expect_segment(const char *name, const char *expected) {
    char *segment = runtime_portable_segment(name);
    if (expected == NULL) {
        if (segment != NULL) {
            fprintf(stderr, "segment \"%s\": expected NULL, got \"%s\"\n", name, segment);
            check_failures++;
        }
    } else {
        check_text(segment, expected, name);
    }
    free(segment);
}

static void segment_rules(void) {
    expect_segment(" repos ", "repos");
    expect_segment("", NULL);
    expect_segment("  ", NULL);
    expect_segment(".", NULL);
    expect_segment("..", NULL);
    expect_segment("a/b", NULL);
    expect_segment("a\\b", NULL);
    expect_segment("a:b", NULL);
    expect_segment("a.", NULL);
    expect_segment("a ", "a");
    expect_segment("con", NULL);
    expect_segment("COM1.txt", NULL);
    expect_segment("lpt9", NULL);
    expect_segment("console", "console");
}

static void resolve_from(const char *names_json, runtime_paths *paths) {
    cJSON *names = names_json == NULL ? NULL : cJSON_Parse(names_json);
    CHECK(runtime_resolve_paths(TEST_HOME, names, paths) == 0);
    cJSON_Delete(names);
}

static void expect_kinds(const runtime_paths *paths, const char *repos, const char *plugin, const char *cache,
                         const char *config) {
    check_text(paths->home, TEST_HOME, "home");
    expect_joined(paths->repos, repos, "repos");
    expect_joined(paths->plugin, plugin, "plugin");
    expect_joined(paths->cache, cache, "cache");
    expect_joined(paths->config, config, "config");
}

static void defaults_when_absent(void) {
    runtime_paths paths;
    resolve_from(NULL, &paths);
    expect_kinds(&paths, "repos", "plugin", "cache", "config");
    runtime_free_paths(&paths);
}

static void declared_names_used(void) {
    runtime_paths paths;
    resolve_from("{\"repos\":\"r\",\"config\":\"settings\"}", &paths);
    expect_kinds(&paths, "r", "plugin", "cache", "settings");
    runtime_free_paths(&paths);
}

static void unusable_name_falls_back(void) {
    runtime_paths paths;
    resolve_from("{\"plugin\":\"../x\",\"cache\":5}", &paths);
    expect_kinds(&paths, "repos", "plugin", "cache", "config");
    runtime_free_paths(&paths);
}

static void duplicates_not_deduplicated(void) {
    runtime_paths paths;
    resolve_from("{\"repos\":\"config\"}", &paths);
    expect_kinds(&paths, "config", "plugin", "cache", "config");
    CHECK(strcmp(paths.repos, paths.config) == 0);
    runtime_free_paths(&paths);
}

static void expect_answer(const cJSON *result, const char *kind, const char *name) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(result, kind);
    CHECK(cJSON_IsString(value));
    if (cJSON_IsString(value)) {
        expect_joined(value->valuestring, name, kind);
    }
}

static void exported_call_round_trip(void) {
    const char *request = "{\"home\":\"" TEST_HOME_JSON "\",\"names\":{\"cache\":\"c\"}}";
    uint8_t *out = NULL;
    size_t out_len = 0;
    int32_t status = runtime_paths_resolve((const uint8_t *)request, strlen(request), &out, &out_len);
    CHECK(status == SPISOR_STATUS_SUCCESS);
    cJSON *result = out == NULL ? NULL : spisor_parse_json(out, out_len);
    CHECK(result != NULL);
    if (result != NULL) {
        check_text(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result, "home")), TEST_HOME, "home");
        expect_answer(result, "repos", "repos");
        expect_answer(result, "cache", "c");
        expect_answer(result, "config", "config");
    }
    cJSON_Delete(result);
    spisor_release(out);
}

static void exported_call_refuses_missing_home(void) {
    const char *request = "{\"names\":{}}";
    uint8_t *out = NULL;
    size_t out_len = 0;
    int32_t status = runtime_paths_resolve((const uint8_t *)request, strlen(request), &out, &out_len);
    CHECK(status == SPISOR_STATUS_FAILURE);
    spisor_release(out);
}

int main(void) {
    segment_rules();
    defaults_when_absent();
    declared_names_used();
    unusable_name_falls_back();
    duplicates_not_deduplicated();
    exported_call_round_trip();
    exported_call_refuses_missing_home();
    return check_summary("paths_test");
}
