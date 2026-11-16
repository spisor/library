#include <string.h>

#include "allocation.h"
#include "check.h"
#include "spisor/json_kind.h"
#include "spisor/json_value.h"

static cJSON *parsed(const char *text) {
    return spisor_parse_json((const uint8_t *)text, strlen(text));
}

static void every_json_kind_becomes_its_node(void) {
    cJSON *json = parsed("{\"name\":\"zweit\\u00e9\",\"count\":42.5,\"on\":true,\"off\":false,\"none\":null,"
                         "\"list\":[1,\"two\",[],{}],\"record\":{\"inner\":{\"deep\":[null]}}}");
    spisor_value_tree tree = SPISOR_INIT(spisor_value_tree);
    const spisor_value *member;
    CHECK(json != NULL);
    CHECK(spisor_value_of_json(json, &tree) == SPISOR_OK);
    cJSON_Delete(json);
    CHECK(tree.root.kind == SPISOR_VALUE_RECORD && tree.root.count == 7);
    CHECK(tree.root.count == 7 && strcmp(tree.root.entries[0].key, "name") == 0 &&
          strcmp(tree.root.entries[6].key, "record") == 0);
    member = spisor_value_member(&tree.root, "name", 4);
    CHECK(member != NULL && member->kind == SPISOR_VALUE_TEXT && member->length == 7 &&
          memcmp(member->text, "zweit\xc3\xa9", 7) == 0);
    member = spisor_value_member(&tree.root, "count", 5);
    CHECK(member != NULL && member->kind == SPISOR_VALUE_NUMBER && member->number == 42.5);
    member = spisor_value_member(&tree.root, "on", 2);
    CHECK(member != NULL && member->kind == SPISOR_VALUE_BOOLEAN && member->boolean == 1);
    member = spisor_value_member(&tree.root, "off", 3);
    CHECK(member != NULL && member->kind == SPISOR_VALUE_BOOLEAN && member->boolean == 0);
    member = spisor_value_member(&tree.root, "none", 4);
    CHECK(member != NULL && member->kind == SPISOR_VALUE_NULL);
    member = spisor_value_member(&tree.root, "list", 4);
    CHECK(member != NULL && member->kind == SPISOR_VALUE_LIST && member->count == 4);
    if (member != NULL && member->count == 4) {
        CHECK(member->items[0].kind == SPISOR_VALUE_NUMBER && member->items[0].number == 1);
        CHECK(member->items[1].kind == SPISOR_VALUE_TEXT && member->items[1].length == 3);
        CHECK(member->items[2].kind == SPISOR_VALUE_LIST && member->items[2].count == 0);
        CHECK(member->items[3].kind == SPISOR_VALUE_RECORD && member->items[3].count == 0);
    }
    member = spisor_value_member(spisor_value_member(&tree.root, "record", 6), "inner", 5);
    member = spisor_value_member(member, "deep", 4);
    CHECK(member != NULL && member->kind == SPISOR_VALUE_LIST && member->count == 1 &&
          member->items[0].kind == SPISOR_VALUE_NULL);
    CHECK(spisor_value_check(&tree.root) == SPISOR_OK);
    spisor_value_release(&tree);
    CHECK(spisor_memory_live_blocks() == 0);
}

static void expect_malformed_json(cJSON *json, const char *label) {
    spisor_value_tree tree = SPISOR_INIT(spisor_value_tree);
    if (spisor_value_of_json(json, &tree) != SPISOR_MALFORMED) {
        fprintf(stderr, "of_json accepted %s\n", label);
        check_failures++;
    }
    CHECK(tree.root.kind == SPISOR_VALUE_NONE && tree.storage == NULL);
    CHECK(spisor_memory_live_blocks() == 0);
    cJSON_Delete(json);
}

static cJSON *nested_arrays(size_t depth) {
    cJSON *root = cJSON_CreateArray();
    cJSON *innermost = root;
    for (size_t level = 0; innermost != NULL && level < depth; level++) {
        cJSON *inner = cJSON_CreateArray();
        cJSON_AddItemToArray(innermost, inner);
        innermost = inner;
    }
    return root;
}

static void what_a_tree_cannot_hold_is_malformed(void) {
    cJSON *twice = cJSON_CreateObject();
    cJSON *bad_text = cJSON_CreateArray();
    cJSON *bad_key = cJSON_CreateObject();
    spisor_value_tree tree = SPISOR_INIT(spisor_value_tree);
    cJSON *deep_enough = nested_arrays(SPISOR_VALUE_DEPTH_LIMIT);
    cJSON_AddNumberToObject(twice, "same", 1);
    cJSON_AddNumberToObject(twice, "same", 2);
    cJSON_AddItemToArray(bad_text, cJSON_CreateString("\xc0\xaf"));
    cJSON_AddNullToObject(bad_key, "\xff");
    expect_malformed_json(twice, "a key held twice");
    expect_malformed_json(bad_text, "a text that is not UTF-8");
    expect_malformed_json(bad_key, "a key that is not UTF-8");
    expect_malformed_json(cJSON_CreateRaw("1"), "a raw item");
    expect_malformed_json(NULL, "NULL");
    expect_malformed_json(nested_arrays(SPISOR_VALUE_DEPTH_LIMIT + 1), "nesting past the limit");
    CHECK(spisor_value_of_json(deep_enough, &tree) == SPISOR_OK);
    spisor_value_release(&tree);
    cJSON_Delete(deep_enough);
}

static void a_tree_that_is_not_empty_is_refused(void) {
    cJSON *json = parsed("[1]");
    spisor_value_tree tree = SPISOR_INIT(spisor_value_tree);
    void *storage;
    CHECK(spisor_value_of_json(json, &tree) == SPISOR_OK);
    storage = tree.storage;
    spisor_memory_fail_at(0);
    CHECK(spisor_value_of_json(json, &tree) == SPISOR_MALFORMED);
    CHECK(spisor_memory_attempts() == 0);
    CHECK(tree.storage == storage);
    spisor_value_release(&tree);
    cJSON_Delete(json);
    CHECK(spisor_memory_live_blocks() == 0);
}

static void every_failed_allocation_leaves_the_tree_empty(void) {
    cJSON *json = parsed("{\"a\":[1,{\"b\":\"text\"}],\"c\":null}");
    for (uint64_t nth = 1; nth < 1000; nth++) {
        spisor_value_tree tree = SPISOR_INIT(spisor_value_tree);
        spisor_status status;
        spisor_memory_fail_at(nth);
        status = spisor_value_of_json(json, &tree);
        if (!spisor_memory_failed()) {
            spisor_memory_fail_at(0);
            CHECK(status == SPISOR_OK);
            spisor_value_release(&tree);
            CHECK(spisor_memory_live_blocks() == 0);
            cJSON_Delete(json);
            return;
        }
        CHECK(spisor_memory_attempts() == nth);
        spisor_memory_fail_at(0);
        CHECK(status == SPISOR_OUT_OF_MEMORY);
        CHECK(tree.root.kind == SPISOR_VALUE_NONE && tree.storage == NULL);
        CHECK(spisor_memory_live_blocks() == 0);
    }
    cJSON_Delete(json);
    CHECK(0);
}

int main(void) {
    every_json_kind_becomes_its_node();
    what_a_tree_cannot_hold_is_malformed();
    a_tree_that_is_not_empty_is_refused();
    every_failed_allocation_leaves_the_tree_empty();
    return check_summary("json_value_test");
}
