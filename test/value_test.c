#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "allocation.h"
#include "check.h"
#include "spisor/value.h"

#define TEXT(literal) text_node(literal, sizeof literal - 1)
#define KEY(literal, node) entry_of(literal, sizeof literal - 1, node)

static spisor_value node_of(spisor_value_kind kind) {
    spisor_value value;
    memset(&value, 0, sizeof value);
    value.kind = (uint32_t)kind;
    return value;
}

static spisor_value boolean_node(uint32_t boolean) {
    spisor_value value = node_of(SPISOR_VALUE_BOOLEAN);
    value.boolean = boolean;
    return value;
}

static spisor_value number_node(double number) {
    spisor_value value = node_of(SPISOR_VALUE_NUMBER);
    value.number = number;
    return value;
}

static spisor_value text_node(const char *text, size_t length) {
    spisor_value value = node_of(SPISOR_VALUE_TEXT);
    value.text = text;
    value.length = length;
    return value;
}

static spisor_value list_node(const spisor_value *items, size_t count) {
    spisor_value value = node_of(SPISOR_VALUE_LIST);
    value.items = items;
    value.count = count;
    return value;
}

static spisor_value record_node(const spisor_entry *entries, size_t count) {
    spisor_value value = node_of(SPISOR_VALUE_RECORD);
    value.entries = entries;
    value.count = count;
    return value;
}

static spisor_entry entry_of(const char *key, size_t key_length, spisor_value value) {
    spisor_entry entry;
    entry.key = key;
    entry.key_length = key_length;
    entry.value = value;
    return entry;
}

typedef struct sample_tree {
    spisor_value tags[3];
    spisor_entry nested[2];
    spisor_entry members[6];
    spisor_value root;
} sample_tree;

static void lay_out_sample(sample_tree *sample) {
    sample->tags[0] = TEXT("first");
    sample->tags[1] = TEXT("zweit\xc3\xa9");
    sample->tags[2] = number_node(3.5);
    sample->nested[0] = KEY("deep", boolean_node(1));
    sample->nested[1] = KEY("empty", list_node(NULL, 0));
    sample->members[0] = KEY("name", TEXT("spisor"));
    sample->members[1] = KEY("count", number_node(42));
    sample->members[2] = KEY("enabled", boolean_node(0));
    sample->members[3] = KEY("nothing", node_of(SPISOR_VALUE_NULL));
    sample->members[4] = KEY("tags", list_node(sample->tags, 3));
    sample->members[5] = KEY("nested", record_node(sample->nested, 2));
    sample->root = record_node(sample->members, 6);
}

static void every_kind_is_well_formed(void) {
    sample_tree sample;
    spisor_value none = node_of(SPISOR_VALUE_NONE);
    spisor_value empty_text = TEXT("");
    spisor_value empty_record = record_node(NULL, 0);
    lay_out_sample(&sample);
    CHECK(spisor_value_check(&sample.root) == SPISOR_OK);
    CHECK(spisor_value_check(&none) == SPISOR_OK);
    CHECK(spisor_value_check(&empty_text) == SPISOR_OK);
    CHECK(spisor_value_check(&empty_record) == SPISOR_OK);
}

static void expect_malformed(spisor_value value, const char *label) {
    if (spisor_value_check(&value) != SPISOR_MALFORMED) {
        fprintf(stderr, "check accepted %s\n", label);
        check_failures++;
    }
}

static void a_broken_node_is_malformed(void) {
    spisor_value with_stray_number = node_of(SPISOR_VALUE_NULL);
    spisor_value text_with_stray_count = TEXT("text");
    spisor_value list_with_entries = list_node(NULL, 0);
    spisor_value unknown = node_of(SPISOR_VALUE_RECORD);
    static const char unterminated[4] = {'a', 'b', 'c', 'd'};
    spisor_entry entry = KEY("key", node_of(SPISOR_VALUE_NULL));
    with_stray_number.number = 1;
    text_with_stray_count.count = 1;
    list_with_entries.entries = &entry;
    unknown.kind = 7;
    CHECK(spisor_value_check(NULL) == SPISOR_MALFORMED);
    expect_malformed(unknown, "an unknown kind");
    expect_malformed(boolean_node(2), "a boolean of 2");
    expect_malformed(number_node(NAN), "a NaN");
    expect_malformed(number_node(INFINITY), "an infinity");
    expect_malformed(text_node(NULL, 0), "a NULL text");
    expect_malformed(text_node(unterminated, 3), "a text without its NUL");
    expect_malformed(text_node("a\0b", 3), "a text holding a NUL");
    expect_malformed(text_node("\xc0\xaf", 2), "a text that is not UTF-8");
    expect_malformed(list_node(NULL, 2), "a list of 2 without items");
    expect_malformed(record_node(NULL, 2), "a record of 2 without entries");
    expect_malformed(with_stray_number, "a null with a number");
    expect_malformed(text_with_stray_count, "a text with a count");
    expect_malformed(list_with_entries, "a list with entries");
}

static void a_broken_member_is_malformed(void) {
    spisor_value none_item[1];
    spisor_entry none_member[1];
    spisor_entry twice[2];
    spisor_entry null_key[1];
    spisor_entry bad_key[1];
    spisor_value nested_bad[1];
    none_item[0] = node_of(SPISOR_VALUE_NONE);
    none_member[0] = KEY("key", node_of(SPISOR_VALUE_NONE));
    twice[0] = KEY("same", number_node(1));
    twice[1] = KEY("same", number_node(2));
    null_key[0] = entry_of(NULL, 0, number_node(1));
    bad_key[0] = entry_of("\xff", 1, number_node(1));
    nested_bad[0] = boolean_node(5);
    expect_malformed(list_node(none_item, 1), "NONE as an item");
    expect_malformed(record_node(none_member, 1), "NONE as a member");
    expect_malformed(record_node(twice, 2), "a key held twice");
    expect_malformed(record_node(null_key, 1), "a NULL key");
    expect_malformed(record_node(bad_key, 1), "a key that is not UTF-8");
    expect_malformed(list_node(nested_bad, 1), "a broken node one level down");
}

static spisor_value *chain_of_lists(size_t depth) {
    spisor_value *nodes = calloc(depth + 1, sizeof *nodes);
    if (nodes == NULL) {
        return NULL;
    }
    for (size_t index = 0; index < depth; index++) {
        nodes[index] = list_node(&nodes[index + 1], 1);
    }
    nodes[depth] = node_of(SPISOR_VALUE_NULL);
    return nodes;
}

static void nesting_stops_at_the_limit(void) {
    spisor_value *at_limit = chain_of_lists(SPISOR_VALUE_DEPTH_LIMIT);
    spisor_value *past_limit = chain_of_lists(SPISOR_VALUE_DEPTH_LIMIT + 1);
    spisor_value loop = node_of(SPISOR_VALUE_LIST);
    CHECK(at_limit != NULL && past_limit != NULL);
    if (at_limit != NULL && past_limit != NULL) {
        CHECK(spisor_value_check(at_limit) == SPISOR_OK);
        CHECK(spisor_value_check(past_limit) == SPISOR_MALFORMED);
    }
    loop.items = &loop;
    loop.count = 1;
    CHECK(spisor_value_check(&loop) == SPISOR_MALFORMED);
    CHECK(!spisor_value_equal(&loop, &loop));
    free(at_limit);
    free(past_limit);
}

static void a_member_is_found_by_its_whole_key(void) {
    sample_tree sample;
    const spisor_value *found;
    lay_out_sample(&sample);
    found = spisor_value_member(&sample.root, "count", 5);
    CHECK(found != NULL && found->kind == SPISOR_VALUE_NUMBER && found->number == 42);
    CHECK(spisor_value_member(&sample.root, "coun", 4) == NULL);
    CHECK(spisor_value_member(&sample.root, "counts", 6) == NULL);
    CHECK(spisor_value_member(&sample.root, "count", 4) == NULL);
    CHECK(spisor_value_member(&sample.tags[0], "first", 5) == NULL);
    CHECK(spisor_value_member(NULL, "count", 5) == NULL);
}

static void equality_reads_records_in_any_order_and_lists_in_order(void) {
    sample_tree sample;
    spisor_entry reordered[2];
    spisor_value forward[2];
    spisor_value backward[2];
    spisor_value record;
    spisor_value forward_list;
    spisor_value backward_list;
    spisor_value shorter_text = text_node("spiso", 5);
    spisor_value longer_text = TEXT("spisor");
    lay_out_sample(&sample);
    reordered[0] = sample.nested[1];
    reordered[1] = sample.nested[0];
    record = record_node(reordered, 2);
    forward[0] = number_node(1);
    forward[1] = number_node(2);
    backward[0] = number_node(2);
    backward[1] = number_node(1);
    forward_list = list_node(forward, 2);
    backward_list = list_node(backward, 2);
    CHECK(spisor_value_equal(&sample.root, &sample.root));
    CHECK(spisor_value_equal(&sample.members[5].value, &record));
    CHECK(!spisor_value_equal(&forward_list, &backward_list));
    CHECK(!spisor_value_equal(&shorter_text, &longer_text));
    CHECK(!spisor_value_equal(&sample.tags[2], &sample.tags[0]));
    CHECK(!spisor_value_equal(&sample.root, &record));
    CHECK(!spisor_value_equal(NULL, &record));
}

static void a_build_outlives_its_source(void) {
    sample_tree sample;
    sample_tree original;
    char name[] = "spisor";
    spisor_value_tree tree = SPISOR_INIT(spisor_value_tree);
    lay_out_sample(&sample);
    lay_out_sample(&original);
    sample.members[0].value = text_node(name, sizeof name - 1);
    spisor_memory_fail_at(0);
    CHECK(spisor_value_build(&sample.root, &tree) == SPISOR_OK);
    CHECK(spisor_memory_attempts() == 1);
    CHECK(tree.storage != NULL);
    CHECK(spisor_value_check(&tree.root) == SPISOR_OK);
    CHECK(spisor_value_equal(&tree.root, &original.root));
    CHECK(tree.root.entries != sample.members);
    CHECK(tree.root.entries[0].value.text != name);
    name[0] = 'X';
    memset(&sample, 0, sizeof sample);
    CHECK(spisor_value_equal(&tree.root, &original.root));
    spisor_value_release(&tree);
    CHECK(tree.size == sizeof tree && tree.storage == NULL && tree.root.kind == SPISOR_VALUE_NONE);
    spisor_value_release(&tree);
    spisor_value_release(NULL);
    CHECK(spisor_memory_live_blocks() == 0);
}

static void a_scalar_builds_without_a_block(void) {
    spisor_value number = number_node(7);
    spisor_value_tree tree = SPISOR_INIT(spisor_value_tree);
    spisor_memory_fail_at(0);
    CHECK(spisor_value_build(&number, &tree) == SPISOR_OK);
    CHECK(spisor_memory_attempts() == 0);
    CHECK(tree.storage == NULL && tree.root.kind == SPISOR_VALUE_NUMBER && tree.root.number == 7);
    spisor_value_release(&tree);
    CHECK(tree.root.kind == SPISOR_VALUE_NONE);
}

static void a_build_refuses_without_allocating(void) {
    spisor_value broken = boolean_node(2);
    spisor_value number = number_node(1);
    spisor_value_tree tree = SPISOR_INIT(spisor_value_tree);
    spisor_value_tree short_tree = {(uint32_t)(sizeof short_tree - 1), {0}, NULL};
    spisor_value_tree filled = SPISOR_INIT(spisor_value_tree);
    spisor_value_tree before;
    CHECK(spisor_value_build(&number, &filled) == SPISOR_OK);
    memcpy(&before, &filled, sizeof before);
    spisor_memory_fail_at(0);
    CHECK(spisor_value_build(&broken, &tree) == SPISOR_MALFORMED);
    CHECK(spisor_value_build(NULL, &tree) == SPISOR_MALFORMED);
    CHECK(spisor_value_build(&number, NULL) == SPISOR_MALFORMED);
    CHECK(spisor_value_build(&number, &short_tree) == SPISOR_MALFORMED);
    CHECK(spisor_value_build(&number, &filled) == SPISOR_MALFORMED);
    CHECK(spisor_memory_attempts() == 0);
    CHECK(tree.root.kind == SPISOR_VALUE_NONE && tree.storage == NULL);
    CHECK(short_tree.root.kind == SPISOR_VALUE_NONE);
    CHECK(memcmp(&before, &filled, sizeof before) == 0);
}

static void every_failed_allocation_leaves_the_tree_empty(void) {
    sample_tree sample;
    lay_out_sample(&sample);
    for (uint64_t nth = 1; nth < 1000; nth++) {
        spisor_value_tree tree = SPISOR_INIT(spisor_value_tree);
        spisor_value_tree before;
        spisor_status status;
        memcpy(&before, &tree, sizeof before);
        spisor_memory_fail_at(nth);
        status = spisor_value_build(&sample.root, &tree);
        if (!spisor_memory_failed()) {
            spisor_memory_fail_at(0);
            CHECK(status == SPISOR_OK);
            spisor_value_release(&tree);
            CHECK(spisor_memory_live_blocks() == 0);
            return;
        }
        CHECK(spisor_memory_attempts() == nth);
        spisor_memory_fail_at(0);
        CHECK(status == SPISOR_OUT_OF_MEMORY);
        CHECK(memcmp(&before, &tree, sizeof before) == 0);
        CHECK(spisor_memory_live_blocks() == 0);
    }
    CHECK(0);
}

int main(void) {
    every_kind_is_well_formed();
    a_broken_node_is_malformed();
    a_broken_member_is_malformed();
    nesting_stops_at_the_limit();
    a_member_is_found_by_its_whole_key();
    equality_reads_records_in_any_order_and_lists_in_order();
    a_build_outlives_its_source();
    a_scalar_builds_without_a_block();
    a_build_refuses_without_allocating();
    every_failed_allocation_leaves_the_tree_empty();
    return check_summary("value_test");
}
