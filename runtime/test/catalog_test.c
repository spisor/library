#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog.h"
#include "catalog_call.h"
#include "catalog_check.h"
#include "catalog_fold.h"
#include "catalog_url.h"
#include "check.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"

#define FIXTURES RUNTIME_CATALOG_FIXTURES_PATH
#define ORIGIN "https://example.com/catalog/index.json"
#define FILE_ORIGIN "file:///C:/builds/index.json"

static char *fixture_text(const char *name, size_t *length) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", FIXTURES, name);
    uint8_t *bytes = spisor_read_file(path, length);
    if (bytes == NULL) {
        fprintf(stderr, "cannot read fixture %s\n", path);
        exit(2);
    }
    return (char *)bytes;
}

static catalog_answer read_index(const char *text, const char *origin) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "text", text);
    cJSON_AddStringToObject(argument, "origin", origin);
    return call_catalog(runtime_catalog_read, argument);
}

static catalog_answer read_fixture(const char *name, const char *origin) {
    size_t length;
    char *text = fixture_text(name, &length);
    catalog_answer answer = read_index(text, origin);
    free(text);
    return answer;
}

static const cJSON *catalog_of(catalog_answer answer, const char *label) {
    const cJSON *catalog = cJSON_GetObjectItemCaseSensitive(answer.tree, "catalog");
    if (answer.status != SPISOR_STATUS_SUCCESS || catalog == NULL) {
        fprintf(stderr, "%s: expected a catalog, got status %d and \"%s\"\n", label, (int)answer.status,
                text_at(answer.tree, "message") == NULL ? "(none)" : text_at(answer.tree, "message"));
        check_failures++;
    }
    return catalog;
}

static char *expected_message(const cJSON *expected) {
    const char *detail = text_at(expected, "detail");
    const char *path = text_at(expected, "path");
    size_t size = strlen(detail) + (path == NULL ? 0 : strlen(path)) + 8;
    char *message = malloc(size);
    snprintf(message, size, "%s%s%s", detail, path != NULL && path[0] != 0 ? " at " : "",
             path != NULL ? path : "");
    return message;
}

static void expect_recorded_refusal(catalog_answer answer, const cJSON *expected, const char *name) {
    const cJSON *c_answer = cJSON_GetObjectItemCaseSensitive(expected, "c");
    const char *prefix = text_at(c_answer, "detailStartsWith");
    const char *fix = text_at(c_answer, "fix") != NULL ? text_at(c_answer, "fix") : text_at(expected, "fix");
    const char *message = text_at(answer.tree, "message");
    if (answer.status != SPISOR_STATUS_FAILURE || message == NULL) {
        fprintf(stderr, "%s: expected a refusal, got status %d\n", name, (int)answer.status);
        check_failures++;
        return;
    }
    if (prefix != NULL) {
        if (strncmp(message, prefix, strlen(prefix)) != 0) {
            fprintf(stderr, "%s: expected a message starting \"%s\", got \"%s\"\n", name, prefix, message);
            check_failures++;
        }
    } else {
        char *wanted = expected_message(expected);
        check_text(message, wanted, name);
        free(wanted);
    }
    check_text(text_at(answer.tree, "fix"), fix, name);
}

static void every_fixture_answers_as_recorded(void) {
    char path[1024];
    snprintf(path, sizeof path, "%s/expected.json", FIXTURES);
    cJSON *recorded = read_json_file(path);
    int count = 0;
    for (const cJSON *expected = cJSON_GetObjectItemCaseSensitive(recorded, "cases")->child; expected != NULL;
         expected = expected->next, count++) {
        const char *name = text_at(expected, "name");
        catalog_answer answer = read_fixture(text_at(expected, "fixture"), text_at(expected, "origin"));
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(expected, "accepted"))) {
            catalog_of(answer, name);
        } else {
            expect_recorded_refusal(answer, expected, name);
        }
        cJSON_Delete(answer.tree);
    }
    CHECK(count >= 140);
    cJSON_Delete(recorded);
}

static void expect_text_at(const cJSON *catalog, const char *slashed, const char *expected, const char *label) {
    const cJSON *value = value_at(catalog, slashed);
    check_text(cJSON_IsString(value) ? value->valuestring : NULL, expected, label);
}

static void it_reads_an_index_of_each_kind(void) {
    catalog_answer answer = read_fixture("reads-an-index-of-each-kind.json", ORIGIN);
    const cJSON *items = value_at(catalog_of(answer, "each kind"), "items");
    const char *order[] = {"a", "b", "c"};
    size_t index = 0;
    for (const cJSON *item = items == NULL ? NULL : items->child; item != NULL; item = item->next, index++) {
        CHECK(index < 3 && strcmp(item->string, order[index]) == 0);
    }
    CHECK(index == 3);
    expect_text_at(items, "b/kind", "host", "each kind");
    cJSON_Delete(answer.tree);
}

static void expect_same_json(const cJSON *actual, const char *expected_json, const char *label) {
    cJSON *expected = parsed(expected_json);
    if (actual == NULL || !cJSON_Compare(actual, expected, 1)) {
        char *printed = actual == NULL ? NULL : cJSON_PrintUnformatted(actual);
        fprintf(stderr, "%s: expected %s, got %s\n", label, expected_json, printed == NULL ? "(none)" : printed);
        cJSON_free(printed);
        check_failures++;
    }
    cJSON_Delete(expected);
}

static void it_reads_an_application_on_a_host_item(void) {
    catalog_answer answer = read_fixture("reads-an-application-on-a-host-item.json", ORIGIN);
    expect_same_json(value_at(catalog_of(answer, "application"), "items/thing/application"),
                     "{\"id\":\"claude-code\",\"label\":\"Claude Code\",\"binary\":\"claude\",\"home\":{\"candidates\":"
                     "[\"~/.claude\"]},\"packages\":{\"npm\":\"@anthropic-ai/claude-code\"}}",
                     "application");
    cJSON_Delete(answer.tree);
}

static void it_reads_a_home_with_an_xdg_subdir(void) {
    catalog_answer answer = read_fixture("reads-a-home-with-an-xdgSubdir.json", ORIGIN);
    expect_same_json(value_at(catalog_of(answer, "xdgSubdir"), "items/thing/application/home"),
                     "{\"candidates\":[\"~/.config/opencode\"],\"xdgSubdir\":\"opencode\"}", "xdgSubdir");
    cJSON_Delete(answer.tree);
}

static void it_reads_an_index_that_starts_with_a_byte_order_mark(void) {
    catalog_answer answer = read_fixture("reads-an-index-that-starts-with-a-UTF-8-byte-order-mark.json", ORIGIN);
    const cJSON *items = value_at(catalog_of(answer, "byte order mark"), "items");
    CHECK(items != NULL && cJSON_GetArraySize(items) == 1 && strcmp(items->child->string, "thing") == 0);
    cJSON_Delete(answer.tree);
}

static void it_resolves_relative_and_absolute_urls(void) {
    catalog_answer https = read_fixture("plugin-item.json", ORIGIN);
    expect_text_at(catalog_of(https, "https origin"), "items/thing/versions/1.0.0/files/plugin.js/url",
                   "https://example.com/catalog/plugin.js", "https origin");
    cJSON_Delete(https.tree);
    catalog_answer file = read_fixture("plugin-item.json", FILE_ORIGIN);
    expect_text_at(catalog_of(file, "file origin"), "items/thing/versions/1.0.0/files/plugin.js/url",
                   "file:///C:/builds/plugin.js", "file origin");
    cJSON_Delete(file.tree);
    catalog_answer absolute = read_fixture("leaves-an-absolute-https-url-alone-under-an-https-origin.json", ORIGIN);
    expect_text_at(catalog_of(absolute, "absolute url"), "items/thing/versions/1.0.0/files/p.js/url",
                   "https://cdn.example.com/p.js", "absolute url");
    cJSON_Delete(absolute.tree);
}

static void it_keeps_members_the_schema_does_not_know(void) {
    catalog_answer answer = read_fixture("accepts-members-the-schema-does-not-know.json", ORIGIN);
    const cJSON *catalog = catalog_of(answer, "unknown members");
    CHECK(cJSON_IsTrue(value_at(catalog, "future")));
    CHECK(cJSON_IsTrue(value_at(catalog, "items/thing/extra/deep")));
    expect_text_at(catalog, "items/thing/versions/1.0.0/files/p.js/mirror", "m", "unknown members");
    cJSON_Delete(answer.tree);
}

static void it_answers_each_item_s_runtime(void) {
    catalog_answer plugin = read_fixture("accepts-a-plugin-item-with-no-runtime.json", ORIGIN);
    expect_text_at(catalog_of(plugin, "plugin default"), "items/thing/runtime", "javascript", "plugin default");
    cJSON_Delete(plugin.tree);
    catalog_answer jvm = read_fixture("accepts-a-plugin-item-that-runs-on-the-jvm.json", ORIGIN);
    expect_text_at(catalog_of(jvm, "plugin jvm"), "items/thing/runtime", "jvm", "plugin jvm");
    cJSON_Delete(jvm.tree);
    catalog_answer kinds = read_fixture("reads-an-index-of-each-kind.json", ORIGIN);
    const cJSON *catalog = catalog_of(kinds, "kinds");
    expect_text_at(catalog, "items/b/runtime", "native", "host runtime");
    CHECK(value_at(catalog, "items/c/runtime") == NULL);
    cJSON_Delete(kinds.tree);
}

static void it_refuses_an_unusable_argument(void) {
    expect_refusal(call_catalog(runtime_catalog_read, parsed("{\"origin\":\"" ORIGIN "\"}")), "text", "no text");
    expect_refusal(call_catalog(runtime_catalog_read, parsed("{\"text\":\"{}\"}")), "origin", "no origin");
}

static void every_url_resolves_as_node_does(void) {
    char path[1024];
    snprintf(path, sizeof path, "%s/urls.json", FIXTURES);
    cJSON *recorded = read_json_file(path);
    for (const cJSON *vector = cJSON_GetObjectItemCaseSensitive(recorded, "vectors")->child; vector != NULL;
         vector = vector->next) {
        const cJSON *c_answer = cJSON_GetObjectItemCaseSensitive(vector, "c");
        const cJSON *expected = cJSON_GetObjectItemCaseSensitive(c_answer != NULL ? c_answer : vector, "resolved");
        char *resolved = runtime_catalog_resolve_within_origin(text_at(vector, "url"), text_at(vector, "origin"));
        const char *wanted = cJSON_IsString(expected) ? expected->valuestring : NULL;
        if ((wanted == NULL) != (resolved == NULL) || (wanted != NULL && strcmp(wanted, resolved) != 0)) {
            fprintf(stderr, "url \"%s\" against \"%s\": expected %s, got %s\n", text_at(vector, "url"),
                    text_at(vector, "origin"), wanted == NULL ? "(refused)" : wanted,
                    resolved == NULL ? "(refused)" : resolved);
            check_failures++;
        }
        free(resolved);
    }
    cJSON_Delete(recorded);
}

static void expect_file_urls(const cJSON *vectors, int windows) {
    for (const cJSON *vector = vectors->child; vector != NULL; vector = vector->next) {
        char *url = runtime_catalog_file_url_of_path(windows, text_at(vector, "path"));
        check_text(url, text_at(vector, "url"), text_at(vector, "path"));
        free(url);
    }
}

static void every_path_becomes_the_file_url_node_writes(void) {
    char path[1024];
    snprintf(path, sizeof path, "%s/file-urls.json", FIXTURES);
    cJSON *recorded = read_json_file(path);
    expect_file_urls(cJSON_GetObjectItemCaseSensitive(recorded, "windows"), 1);
    expect_file_urls(cJSON_GetObjectItemCaseSensitive(recorded, "posix"), 0);
    cJSON_Delete(recorded);
}

static void every_pair_folds_as_forebay_host_folded(void) {
    char path[1024];
    snprintf(path, sizeof path, "%s/folds.json", FIXTURES);
    cJSON *recorded = read_json_file(path);
    for (const cJSON *vector = cJSON_GetObjectItemCaseSensitive(recorded, "vectors")->child; vector != NULL;
         vector = vector->next) {
        const char *keys[] = {text_at(vector, "first"), text_at(vector, "second")};
        size_t first = 9;
        size_t second = 9;
        int collides = runtime_catalog_folded_duplicate(keys, 2, &first, &second);
        int expected = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(vector, "collide"));
        if (collides != expected || (collides && (first != 0 || second != 1))) {
            fprintf(stderr, "fold of \"%s\" and \"%s\": expected %d, got %d\n", keys[0], keys[1], expected, collides);
            check_failures++;
        }
    }
    cJSON_Delete(recorded);
}

static void it_names_the_earliest_folded_pair(void) {
    const char *keys[] = {"a.js", "B.js", "c.js", "b.js", "A.js"};
    size_t first = 9;
    size_t second = 9;
    CHECK(runtime_catalog_folded_duplicate(keys, 5, &first, &second) == 1);
    CHECK(first == 1 && second == 3);
}

static const cJSON *schema_value_at(const cJSON *schema, const char *pointer) {
    char path[512];
    snprintf(path, sizeof path, "%s", pointer + 1);
    return value_at(schema, path);
}

static int enum_holds(const cJSON *values, const char *value) {
    for (const cJSON *entry = values == NULL ? NULL : values->child; entry != NULL; entry = entry->next) {
        if (cJSON_IsString(entry) && strcmp(entry->valuestring, value) == 0) {
            return 1;
        }
    }
    return 0;
}

static void the_checker_holds_what_the_schema_says(void) {
    cJSON *schema = read_json_file(RUNTIME_CATALOG_SCHEMA_PATH);
    for (size_t index = 0; index < runtime_catalog_schema_pattern_count; index++) {
        const runtime_catalog_schema_value *pattern = &runtime_catalog_schema_patterns[index];
        const cJSON *value = schema_value_at(schema, pattern->pointer);
        check_text(cJSON_IsString(value) ? value->valuestring : NULL, pattern->value, pattern->pointer);
    }
    for (size_t index = 0; index < runtime_catalog_schema_enum_value_count; index++) {
        const runtime_catalog_schema_value *value = &runtime_catalog_schema_enum_values[index];
        const cJSON *values = schema_value_at(schema, value->pointer);
        if (!enum_holds(values, value->value)) {
            fprintf(stderr, "%s does not hold %s\n", value->pointer, value->value);
            check_failures++;
        }
    }
    CHECK(cJSON_GetArraySize(schema_value_at(schema, "/definitions/item/properties/kind/enum")) == 3);
    CHECK(cJSON_GetArraySize(schema_value_at(schema, "/definitions/item/properties/runtime/enum")) == 3);
    cJSON_Delete(schema);
}

int main(void) {
    every_fixture_answers_as_recorded();
    it_reads_an_index_of_each_kind();
    it_reads_an_application_on_a_host_item();
    it_reads_a_home_with_an_xdg_subdir();
    it_reads_an_index_that_starts_with_a_byte_order_mark();
    it_resolves_relative_and_absolute_urls();
    it_keeps_members_the_schema_does_not_know();
    it_answers_each_item_s_runtime();
    it_refuses_an_unusable_argument();
    every_url_resolves_as_node_does();
    every_path_becomes_the_file_url_node_writes();
    every_pair_folds_as_forebay_host_folded();
    it_names_the_earliest_folded_pair();
    the_checker_holds_what_the_schema_says();
    return check_summary("catalog_test");
}
