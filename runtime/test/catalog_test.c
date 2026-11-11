#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog.h"
#include "catalog_call.h"
#include "catalog_check.h"
#include "catalog_fold.h"
#include "catalog_schema.h"
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
    const char *prefix = text_at(expected, "detailStartsWith");
    const char *fix = text_at(expected, "fix");
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
        const cJSON *expected = cJSON_GetObjectItemCaseSensitive(vector, "resolved");
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

static void every_pair_folds_as_spisor_host_folded(void) {
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

typedef struct schema_walk {
    size_t found[8];
} schema_walk;

static const char *const KEYWORDS[] = {"pattern", "required", "enum", "const", "minLength", "minimum", "minItems",
                                       "properties"};
#define KEYWORD_COUNT (sizeof KEYWORDS / sizeof KEYWORDS[0])

static int ends_with(const char *text, const char *suffix) {
    size_t length = strlen(text);
    size_t suffix_length = strlen(suffix);
    return length >= suffix_length && strcmp(text + length - suffix_length, suffix) == 0;
}

static int same_names(const cJSON *array, const char *const *names) {
    size_t index = 0;
    for (const cJSON *entry = array->child; entry != NULL; entry = entry->next, index++) {
        if (!cJSON_IsString(entry) || names[index] == NULL || strcmp(entry->valuestring, names[index]) != 0) {
            return 0;
        }
    }
    return names[index] == NULL;
}

static int same_property_names(const cJSON *properties, const runtime_catalog_property_rule *rules) {
    size_t index = 0;
    for (const cJSON *entry = properties->child; entry != NULL; entry = entry->next, index++) {
        if (rules[index].name == NULL || strcmp(entry->string, rules[index].name) != 0) {
            return 0;
        }
    }
    return rules[index].name == NULL;
}

static int held_list(const char *pointer, const cJSON *value) {
    for (size_t index = 0; index < LIST_COUNT; index++) {
        if (strcmp(runtime_catalog_schema_lists[index].pointer, pointer) == 0) {
            return same_names(value, runtime_catalog_schema_lists[index].values);
        }
    }
    return 0;
}

static int held_text(const runtime_catalog_schema_text *table, size_t count, const char *pointer, const cJSON *value,
                     int as_json) {
    for (size_t index = 0; index < count; index++) {
        if (strcmp(table[index].pointer, pointer) != 0) {
            continue;
        }
        if (!as_json) {
            return cJSON_IsString(value) && strcmp(value->valuestring, table[index].value) == 0;
        }
        cJSON *constant = parsed(table[index].value);
        int equal = cJSON_Compare(value, constant, 1);
        cJSON_Delete(constant);
        return equal;
    }
    return 0;
}

static int held_bound(const char *pointer, const cJSON *value) {
    for (size_t index = 0; index < BOUND_COUNT; index++) {
        if (strcmp(runtime_catalog_schema_bounds[index].pointer, pointer) == 0) {
            return cJSON_IsNumber(value) && value->valuedouble == runtime_catalog_schema_bounds[index].value;
        }
    }
    return 0;
}

static int held_properties(const char *pointer, const cJSON *value) {
    for (size_t index = 0; index < runtime_catalog_schema_property_set_count; index++) {
        if (strcmp(runtime_catalog_schema_property_sets[index].pointer, pointer) == 0) {
            return same_property_names(value, runtime_catalog_schema_property_sets[index].rules);
        }
    }
    return 0;
}

/** Whether the checker holds the keyword at pointer as the schema says it. */
static int held(size_t keyword, const char *pointer, const cJSON *value) {
    switch (keyword) {
    case 0:
        return held_text(runtime_catalog_schema_patterns, PATTERN_COUNT, pointer, value, 0);
    case 1:
    case 2:
        return held_list(pointer, value);
    case 3:
        return held_text(runtime_catalog_schema_constants, CONSTANT_COUNT, pointer, value, 1);
    case 7:
        return held_properties(pointer, value);
    default:
        return held_bound(pointer, value);
    }
}

static void visit_keyword(schema_walk *walk, const char *pointer, const cJSON *member) {
    for (size_t keyword = 0; keyword < KEYWORD_COUNT; keyword++) {
        int is_if_clause = keyword == 7 && strstr(pointer, "/if/") != NULL;
        if (strcmp(member->string, KEYWORDS[keyword]) != 0 || is_if_clause) {
            continue;
        }
        walk->found[keyword]++;
        if (!held(keyword, pointer, member)) {
            fprintf(stderr, "the checker does not hold %s as the schema says it\n", pointer);
            check_failures++;
        }
    }
}

static void walk_schema(schema_walk *walk, const cJSON *node, const char *pointer) {
    int index = 0;
    for (const cJSON *child = node->child; child != NULL; child = child->next, index++) {
        char path[512];
        if (cJSON_IsArray(node)) {
            snprintf(path, sizeof path, "%s/%d", pointer, index);
        } else {
            snprintf(path, sizeof path, "%s/%s", pointer, child->string);
            visit_keyword(walk, path, child);
        }
        if (cJSON_IsObject(child) || cJSON_IsArray(child)) {
            walk_schema(walk, child, path);
        }
    }
}

static size_t held_count(size_t keyword) {
    size_t count = 0;
    if (keyword == 0) {
        return PATTERN_COUNT;
    }
    if (keyword == 3) {
        return CONSTANT_COUNT;
    }
    if (keyword == 7) {
        return runtime_catalog_schema_property_set_count;
    }
    for (size_t index = 0; keyword <= 2 && index < LIST_COUNT; index++) {
        count += ends_with(runtime_catalog_schema_lists[index].pointer, keyword == 1 ? "/required" : "/enum");
    }
    for (size_t index = 0; keyword >= 4 && index < BOUND_COUNT; index++) {
        char suffix[32];
        snprintf(suffix, sizeof suffix, "/%s", KEYWORDS[keyword]);
        count += ends_with(runtime_catalog_schema_bounds[index].pointer, suffix);
    }
    return count;
}

static void the_checker_holds_what_the_schema_says(void) {
    cJSON *schema = read_json_file(RUNTIME_CATALOG_SCHEMA_PATH);
    schema_walk walk;
    memset(&walk, 0, sizeof walk);
    walk_schema(&walk, schema, "");
    for (size_t keyword = 0; keyword < KEYWORD_COUNT; keyword++) {
        if (walk.found[keyword] != held_count(keyword)) {
            fprintf(stderr, "the schema has %zu %s, the checker holds %zu\n", walk.found[keyword], KEYWORDS[keyword],
                    held_count(keyword));
            check_failures++;
        }
    }
    const cJSON *rules = value_at(schema, "definitions/item/allOf");
    CHECK(cJSON_GetArraySize(rules) == RUNTIME_CATALOG_ITEM_RULE_COUNT);
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
    every_pair_folds_as_spisor_host_folded();
    it_names_the_earliest_folded_pair();
    the_checker_holds_what_the_schema_says();
    return check_summary("catalog_test");
}
