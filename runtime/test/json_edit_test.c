#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "json_edit.h"
#include "spisor/json_kind.h"

static cJSON *parsed(const char *json) {
    cJSON *tree = cJSON_Parse(json);
    CHECK(tree != NULL);
    return tree;
}

/** json_edit_set on text, compared with expected; NULL expected means the edit must refuse. */
static void expect_set(const char *text, const char *key_json, const char *value_json, const char *expected,
                       const char *label) {
    cJSON *key = parsed(key_json);
    cJSON *value = parsed(value_json);
    json_edit_text edited = {NULL, 0, {0, 0, 0}};
    int done = json_edit_set(text, strlen(text), key, value, &edited);
    if (expected == NULL) {
        CHECK(!done);
    } else if (done) {
        CHECK(edited.length == strlen(edited.bytes));
        check_text(edited.bytes, expected, label);
    } else {
        check_text("(refused)", expected, label);
    }
    free(edited.bytes);
    cJSON_Delete(key);
    cJSON_Delete(value);
}

static void expect_remove(const char *text, const char *key_json, int created_from, const char *expected,
                          const char *label) {
    cJSON *key = parsed(key_json);
    json_edit_text edited = {NULL, 0, {0, 0, 0}};
    int done = json_edit_remove(text, strlen(text), key, created_from, &edited);
    check_text(done ? edited.bytes : "(refused)", expected, label);
    free(edited.bytes);
    cJSON_Delete(key);
}

/** Setting then removing with the createdFrom the apply would record answers the original bytes. */
static void expect_round_trip(const char *text, const char *key_json, const char *value_json, int created_from,
                              const char *label) {
    cJSON *key = parsed(key_json);
    cJSON *value = parsed(value_json);
    json_edit_text set = {NULL, 0, {0, 0, 0}};
    json_edit_text removed = {NULL, 0, {0, 0, 0}};
    CHECK(json_edit_set(text, strlen(text), key, value, &set));
    CHECK(set.bytes != NULL && json_edit_remove(set.bytes, set.length, key, created_from, &removed));
    check_text(removed.bytes, text, label);
    free(set.bytes);
    free(removed.bytes);
    cJSON_Delete(key);
    cJSON_Delete(value);
}

static const char *const odd_form = "{\"hooks\":{\"SessionStart\":[{\"hooks\":[{\"type\":\"command\",\"command\":"
                                    "\"echo hi\"}]}]},\n    \"enabledPlugins\": { \"other@market\": true },  "
                                    "\"n\": 1.50, \"u\": \"\\u00e9\" }\n";

static void a_present_value_has_only_its_bytes_replaced(void) {
    expect_set("{\"a\":  1.50 ,\r\n \"b\": \"\\u00e9\"}", "[\"b\"]", "true",
               "{\"a\":  1.50 ,\r\n \"b\": true}", "replace in an odd file");
    expect_set("{\n  \"x\": {\n    \"y\": 1\n  }\n}\n", "[\"x\",\"y\"]", "{\"z\":[1]}",
               "{\n  \"x\": {\n    \"y\": {\n      \"z\": [\n        1\n      ]\n    }\n  }\n}\n",
               "replace two levels deep in the file's style");
}

static void an_absent_member_is_added_last_in_its_objects_style(void) {
    expect_set("{\n    \"a\": 1\n}\n", "[\"b\"]", "2", "{\n    \"a\": 1,\n    \"b\": 2\n}\n", "four spaces");
    expect_set("{\r\n  \"a\": 1\r\n}\r\n", "[\"b\"]", "{\"c\":true}",
               "{\r\n  \"a\": 1,\r\n  \"b\": {\r\n    \"c\": true\r\n  }\r\n}\r\n", "CRLF");
    expect_set("{\n\t\"a\": 1\n}", "[\"b\"]", "[1,2]", "{\n\t\"a\": 1,\n\t\"b\": [\n\t\t1,\n\t\t2\n\t]\n}", "tabs");
    expect_set("{ \"a\": 1 }", "[\"b\"]", "{\"c\":2}", "{ \"a\": 1, \"b\": {\"c\":2} }", "one line");
    expect_set("{\"a\":1}", "[\"b\"]", "2", "{\"a\":1,\"b\":2}", "compact");
    expect_set(odd_form, "[\"enabledPlugins\",\"spisor@spisor\"]", "true",
               "{\"hooks\":{\"SessionStart\":[{\"hooks\":[{\"type\":\"command\",\"command\":\"echo hi\"}]}]},\n"
               "    \"enabledPlugins\": { \"other@market\": true, \"spisor@spisor\": true },  \"n\": 1.50, "
               "\"u\": \"\\u00e9\" }\n",
               "the probe's odd file");
}

static void missing_objects_are_created_on_the_way(void) {
    expect_set("{\n  \"a\": 1\n}\n", "[\"m\",\"n\",\"o\"]", "\"v\"",
               "{\n  \"a\": 1,\n  \"m\": {\n    \"n\": {\n      \"o\": \"v\"\n    }\n  }\n}\n", "nested");
    expect_set("{\n  \"a\": {\n    \"b\": 1\n  }\n}", "[\"a\",\"c\",\"d\"]", "1",
               "{\n  \"a\": {\n    \"b\": 1,\n    \"c\": {\n      \"d\": 1\n    }\n  }\n}", "inside a present one");
}

static void an_empty_object_takes_the_files_style(void) {
    expect_set("{\n  \"a\": {}\n}\n", "[\"a\",\"k\"]", "1", "{\n  \"a\": {\n    \"k\": 1\n  }\n}\n", "nested empty");
    expect_set("{\r\n    \"a\": { }\r\n}", "[\"a\",\"k\"]", "1", "{\r\n    \"a\": { \r\n        \"k\": 1\r\n    }\r\n}",
               "nested empty CRLF keeps the space inside");
    expect_set("{\n  \"a\": {\n    // x\n  }\n}\n", "[\"a\",\"k\"]", "1",
               "{\n  \"a\": {\n    // x\n    \"k\": 1\n  }\n}\n", "a comment inside an empty object is kept");
    expect_set("{\r\n  \"a\": {\r\n  }\r\n}", "[\"a\",\"k\"]", "1", "{\r\n  \"a\": {\r\n    \"k\": 1\r\n  }\r\n}",
               "an empty object over two CRLF lines");
    expect_set("{ }", "[\"k\"]", "1", "{\"k\":1 }", "a compact empty object keeps its space");
    expect_set("{}", "[\"k\"]", "{\"x\":1}", "{\"k\":{\"x\":1}}", "a bare empty file is compact");
    expect_set("{}\n", "[\"k\",\"j\"]", "1", "{\"k\":{\"j\":1}}\n", "compact missing objects");
}

static void a_key_is_matched_by_what_it_decodes_to(void) {
    expect_set("{\"\\u0061\": 1}", "[\"a\"]", "2", "{\"\\u0061\": 2}", "an escaped key");
    expect_set("{\"a\": 1, \"a\": 2}", "[\"a\"]", "3", "{\"a\": 1, \"a\": 3}", "the last of a duplicated key");
    expect_set("{\"a b\\\"\": 1}", "[\"a b\\\"\"]", "2", "{\"a b\\\"\": 2}", "a key holding a quote");
}

static void a_string_escapes_only_what_json_requires(void) {
    expect_set("{}", "[\"k\"]", "\"\xc3\xa9 \\\" \\n\"", "{\"k\":\"\xc3\xa9 \\\" \\n\"}", "raw UTF-8 kept");
    expect_set("{}", "[\"k\\\"\"]", "1", "{\"k\\\"\":1}", "a key with a quote is escaped");
}

static void comments_and_strings_do_not_confuse_the_scanner(void) {
    expect_set("{\n  // a note\n  \"a\": \"} , {\"\n}\n", "[\"b\"]", "1",
               "{\n  // a note\n  \"a\": \"} , {\",\n  \"b\": 1\n}\n", "comment and braces in a string");
}

static void the_scanner_reads_every_shape_of_token(void) {
    expect_set("\xEF\xBB\xBF{\"a\": 1}", "[\"b\"]", "2", "\xEF\xBB\xBF{\"a\": 1,\"b\": 2}", "a BOM");
    expect_set("{\"n\": 1E+5, \"m\": -1e-3}", "[\"m\"]", "0", "{\"n\": 1E+5, \"m\": 0}", "exponent numbers");
    expect_set("{\"n\": -1e-3}", "[\"x\"]", "1", "{\"n\": -1e-3,\"x\": 1}", "an exponent last");
    expect_set("{\"list\": [{\"t\": \"}{][\"}, {\"u\": \"\\\\\\\"}\"}], \"z\": 1}", "[\"z\"]", "2",
               "{\"list\": [{\"t\": \"}{][\"}, {\"u\": \"\\\\\\\"}\"}], \"z\": 2}", "objects in an array, braces in strings");
    expect_set("{\"list\": [{\"t\": \"}\"}]}", "[\"list\",\"t\"]", "1", NULL, "an array is no object on the way");
}

static void a_duplicated_key_resolves_as_json_parse_does(void) {
    const char *text = "{\"a\":1,\"a\":2}";
    cJSON *key = parsed("[\"a\"]");
    json_span value = {0, 0};
    CHECK(json_edit_find_value(text, strlen(text), key, &value) && value.start == 11 && value.end == 12);
    cJSON *read = spisor_parse_json((const uint8_t *)text, strlen(text));
    CHECK(cJSON_GetObjectItemCaseSensitive(read, "a")->valueint == 2);
    cJSON_Delete(read);
    cJSON_Delete(key);
    expect_set(text, "[\"a\"]", "3", "{\"a\":1,\"a\":3}", "the last duplicate is set");
}

static void a_malformed_file_or_path_is_refused(void) {
    expect_set("{ not json", "[\"a\"]", "1", NULL, "malformed");
    expect_set("[1, 2]", "[\"a\"]", "1", NULL, "not an object");
    expect_set("{\"a\": 3}", "[\"a\",\"b\"]", "1", NULL, "a key on the way is no object");
    expect_set("{\"a\": 1} x", "[\"a\"]", "2", NULL, "trailing bytes");
}

static void removal_undoes_an_add_exactly(void) {
    expect_round_trip(odd_form, "[\"extraKnownMarketplaces\",\"spisor\"]",
                      "{\"source\":{\"source\":\"directory\",\"path\":\"/x\"}}", 0, "odd file, missing object");
    expect_round_trip("{\r\n  \"a\": 1\r\n}", "[\"b\"]", "2", 0, "CRLF and no trailing newline");
    expect_round_trip("{\n  \"a\": {}\n}\n", "[\"a\",\"k\"]", "1", 1, "an empty object comes back empty");
    expect_round_trip("{}", "[\"k\",\"j\"]", "1", 0, "a bare empty file");
}

static void removal_keeps_what_the_user_added_beside_it(void) {
    expect_remove("{\n  \"a\": 1,\n  \"ours\": 2,\n  \"theirs\": 3\n}\n", "[\"ours\"]", 0,
                  "{\n  \"a\": 1,\n  \"theirs\": 3\n}\n", "a member after ours");
    expect_remove("{\n  \"ours\": 2,\n  \"theirs\": 3\n}\n", "[\"ours\"]", 0, "{\n  \"theirs\": 3\n}\n",
                  "ours first");
    expect_remove("{\n  \"m\": {\n    \"ours\": 1,\n    \"theirs\": 2\n  }\n}\n", "[\"m\",\"ours\"]", 0,
                  "{\n  \"m\": {\n    \"theirs\": 2\n  }\n}\n", "a created object the user added to stays");
    expect_remove("{\n  \"a\": 1\n}\n", "[\"gone\"]", 0, "{\n  \"a\": 1\n}\n", "an absent member");
}

static void a_value_is_found_and_replaced_by_its_bytes(void) {
    const char *text = "{\"a\": {\"b\": 1.50}}";
    cJSON *key = parsed("[\"a\",\"b\"]");
    json_span value = {0, 0};
    CHECK(json_edit_find_value(text, strlen(text), key, &value) && value.start == 12 && value.end == 16);
    json_edit_text edited = {NULL, 0, {0, 0, 0}};
    CHECK(json_edit_replace_value(text, strlen(text), key, "\"x\"", 3, &edited));
    check_text(edited.bytes, "{\"a\": {\"b\": \"x\"}}", "replace by bytes");
    free(edited.bytes);
    cJSON_Delete(key);
    CHECK(json_edit_is_empty_object(" { }\n", 5) && !json_edit_is_empty_object("{\"a\":1}", 7));
}

static void a_new_document_is_stringify_form(void) {
    cJSON *key = parsed("[\"a\",\"b\"]");
    cJSON *value = parsed("true");
    json_edit_text created = {NULL, 0, {0, 0, 0}};
    CHECK(json_edit_new_document(key, value, &created));
    check_text(created.bytes, "{\n  \"a\": {\n    \"b\": true\n  }\n}\n", "new document");
    free(created.bytes);
    cJSON_Delete(key);
    cJSON_Delete(value);
}

int main(void) {
    a_present_value_has_only_its_bytes_replaced();
    an_absent_member_is_added_last_in_its_objects_style();
    missing_objects_are_created_on_the_way();
    an_empty_object_takes_the_files_style();
    a_key_is_matched_by_what_it_decodes_to();
    a_string_escapes_only_what_json_requires();
    comments_and_strings_do_not_confuse_the_scanner();
    the_scanner_reads_every_shape_of_token();
    a_duplicated_key_resolves_as_json_parse_does();
    a_malformed_file_or_path_is_refused();
    removal_undoes_an_add_exactly();
    removal_keeps_what_the_user_added_beside_it();
    a_value_is_found_and_replaced_by_its_bytes();
    a_new_document_is_stringify_form();
    return check_summary("json_edit_test");
}
