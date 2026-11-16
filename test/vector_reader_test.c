#include <stdio.h>
#include <string.h>

#include "check.h"
#include "spisor/value.h"
#include "vector_reader.h"

static void a_text_is_borrowed_with_its_length(const cJSON *vector) {
    const char *text = NULL;
    size_t length = 0;
    CHECK(spisor_vector_text(vector, "name", &text, &length));
    CHECK(length == 7 && text != NULL && memcmp(text, "zweit\xc3\xa9", 7) == 0);
    CHECK(!spisor_vector_text(vector, "number", &text, &length));
    CHECK(!spisor_vector_text(vector, "absent", &text, &length));
}

static void every_status_is_read_by_its_name(const cJSON *vector) {
    const cJSON *statuses = spisor_vector_member(vector, "statuses");
    spisor_status status = SPISOR_OK;
    for (int number = 0; number <= SPISOR_OUT_OF_MEMORY; number++) {
        char key[2] = {(char)('0' + number), '\0'};
        CHECK(spisor_vector_status(statuses, key, &status) && (int)status == number);
    }
    CHECK(!spisor_vector_status(vector, "unknown status", &status));
    CHECK(!spisor_vector_status(vector, "number", &status));
}

static void a_refusal_is_borrowed_and_owns_nothing(const cJSON *vector) {
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_refusal without_fix = SPISOR_INIT(spisor_refusal);
    spisor_refusal unread = SPISOR_INIT(spisor_refusal);
    CHECK(spisor_vector_refusal(vector, "refusal", &refusal));
    check_text(refusal.detail, "the settings file cannot be read", "detail");
    check_text(refusal.fix, "check its permissions", "fix");
    CHECK(refusal.size == sizeof refusal && refusal.storage == NULL);
    CHECK(spisor_vector_refusal(vector, "refusal without fix", &without_fix));
    check_text(without_fix.detail, "no such key", "detail");
    CHECK(without_fix.fix == NULL);
    CHECK(!spisor_vector_refusal(vector, "refusal with a number", &unread));
    CHECK(!spisor_vector_refusal(vector, "name", &unread));
    CHECK(unread.detail == NULL && unread.fix == NULL);
}

static void a_value_becomes_a_tree(const cJSON *vector) {
    spisor_value_tree tree = SPISOR_INIT(spisor_value_tree);
    spisor_value tags[2];
    spisor_entry empty[1];
    spisor_entry members[4];
    spisor_value expected;
    memset(tags, 0, sizeof tags);
    memset(empty, 0, sizeof empty);
    memset(members, 0, sizeof members);
    memset(&expected, 0, sizeof expected);
    tags[0].kind = SPISOR_VALUE_TEXT;
    tags[0].text = "a";
    tags[0].length = 1;
    tags[1].kind = SPISOR_VALUE_NULL;
    empty[0].key = "empty";
    empty[0].key_length = 5;
    empty[0].value.kind = SPISOR_VALUE_RECORD;
    members[0].key = "enabled";
    members[0].key_length = 7;
    members[0].value.kind = SPISOR_VALUE_BOOLEAN;
    members[0].value.boolean = 1;
    members[1].key = "count";
    members[1].key_length = 5;
    members[1].value.kind = SPISOR_VALUE_NUMBER;
    members[1].value.number = 2.5;
    members[2].key = "tags";
    members[2].key_length = 4;
    members[2].value.kind = SPISOR_VALUE_LIST;
    members[2].value.items = tags;
    members[2].value.count = 2;
    members[3].key = "nested";
    members[3].key_length = 6;
    members[3].value.kind = SPISOR_VALUE_RECORD;
    members[3].value.entries = empty;
    members[3].value.count = 1;
    expected.kind = SPISOR_VALUE_RECORD;
    expected.entries = members;
    expected.count = 4;
    CHECK(spisor_vector_value(vector, "value", &tree));
    CHECK(spisor_value_equal(&tree.root, &expected));
    spisor_value_release(&tree);
    CHECK(!spisor_vector_value(vector, "absent", &tree));
    CHECK(tree.root.kind == SPISOR_VALUE_NONE && tree.storage == NULL);
}

int main(void) {
    cJSON *vector = spisor_vector_read(SPISOR_VECTOR_READER_VECTOR_PATH);
    CHECK(vector != NULL);
    CHECK(spisor_vector_read(SPISOR_VECTOR_READER_VECTOR_PATH ".absent") == NULL);
    if (vector != NULL) {
        a_text_is_borrowed_with_its_length(vector);
        every_status_is_read_by_its_name(vector);
        a_refusal_is_borrowed_and_owns_nothing(vector);
        a_value_becomes_a_tree(vector);
        cJSON_Delete(vector);
    }
    return check_summary("vector_reader_test");
}
