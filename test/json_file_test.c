#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "check.h"
#include "spisor/file.h"
#include "spisor/json_file.h"

#define SCRATCH_FILE SPISOR_SCRATCH_PATH "/json_file_test.json"

static void expect_stripped(const char *input, const char *expected, const char *label) {
    size_t stripped_length = 0;
    char *stripped = spisor_strip_comment_lines(input, strlen(input), &stripped_length);
    CHECK(stripped != NULL);
    if (stripped != NULL) {
        check_text(stripped, expected, label);
        CHECK(stripped_length == strlen(expected));
    }
    free(stripped);
}

static void strips_whole_line_comments(void) {
    expect_stripped("// head\n{\n    // indented\n\"a\":1,\n\t// tabbed\n\"b\":2\n}\n// tail",
                    "\n{\n\n\"a\":1,\n\n\"b\":2\n}\n", "whole-line comments");
}

static void keeps_a_comment_that_follows_content(void) {
    expect_stripped("{} // trailing", "{} // trailing", "a trailing comment");
}

static void keeps_slashes_inside_a_string(void) {
    expect_stripped("{\"k\":\"http://x\",\"j\":\"//share\"}", "{\"k\":\"http://x\",\"j\":\"//share\"}", "slashes in strings");
}

static void strips_comments_in_a_crlf_text(void) {
    expect_stripped("{\r\n  // one\r\n  \"k\":1\r\n}\r\n", "{\r\n  \"k\":1\r\n}\r\n", "crlf comments");
}

static void starts_a_comment_line_after_a_lone_cr(void) {
    expect_stripped("{\"k\":1,\r// gone\n\"j\":2}", "{\"k\":1,\r\n\"j\":2}", "a cr starts a line");
}

static void starts_a_comment_line_after_a_line_separator(void) {
    expect_stripped("a\xE2\x80\xA8// cut", "a\xE2\x80\xA8", "u+2028 then //");
}

static void keeps_a_line_separator_without_a_comment(void) {
    expect_stripped("a\xE2\x80\xA8z", "a\xE2\x80\xA8z", "u+2028 alone");
}

static void strips_a_comment_led_by_javascript_whitespace(void) {
    expect_stripped("{\n\xC2\xA0\xE3\x80\x80// wide\n}", "{\n\n}", "no-break and ideographic spaces");
    expect_stripped("{\n\xE2\x80\x83\xEF\xBB\xBF// ranged\n}", "{\n\n}", "u+2003 and a bom");
}

static void strips_blank_lines_up_to_a_comment(void) {
    expect_stripped("{\n\v\n\t\f\n// eaten\n\"k\":1}", "{\n\n\"k\":1}", "vertical tab and form feed on earlier lines");
    expect_stripped("{\n\v\n\"k\":1}", "{\n\v\n\"k\":1}", "the same whitespace with no comment");
}

static void strips_a_bom_that_leads_a_comment(void) {
    expect_stripped("\xEF\xBB\xBF// c\n{}", "\n{}", "bom eaten by the comment whitespace");
}

static void strips_a_comment_holding_a_nul(void) {
    static const char text[] = "// \xC3 and a \0 nul\n{}";
    size_t stripped_length = 0;
    char *stripped = spisor_strip_comment_lines(text, sizeof(text) - 1, &stripped_length);
    CHECK(stripped != NULL && stripped_length == 3 && memcmp(stripped, "\n{}", 3) == 0);
    free(stripped);
}

static cJSON *read_content(const char *content, size_t length, spisor_json_file_state *state) {
    CHECK(spisor_replace_file(SCRATCH_FILE, (const uint8_t *)content, length));
    return spisor_read_json_file(SCRATCH_FILE, state);
}

static void read_absent_file(void) {
    spisor_json_file_state state = SPISOR_JSON_FILE_PARSED;
    cJSON *tree = spisor_read_json_file(SPISOR_SCRATCH_PATH "/no-such-file.json", &state);
    CHECK(tree == NULL);
    CHECK(state == SPISOR_JSON_FILE_ABSENT);
}

static void read_beneath_a_file(void) {
    spisor_json_file_state state = SPISOR_JSON_FILE_PARSED;
    CHECK(spisor_replace_file(SCRATCH_FILE, (const uint8_t *)"{}", 2));
    cJSON *tree = spisor_read_json_file(SCRATCH_FILE "/inner.json", &state);
    CHECK(tree == NULL);
    CHECK(state == SPISOR_JSON_FILE_ABSENT);
}

#if defined(_WIN32)
static void read_where_the_os_will_not_answer(void) {
    spisor_json_file_state state = SPISOR_JSON_FILE_PARSED;
    cJSON *tree = spisor_read_json_file(SPISOR_SCRATCH_PATH "/in<valid.json", &state);
    CHECK(tree == NULL);
    CHECK(state == SPISOR_JSON_FILE_UNREADABLE);
}
#else
static void read_where_the_os_will_not_answer(void) {
    const char *sealed = SPISOR_SCRATCH_PATH "/sealed";
    const char *inside = SPISOR_SCRATCH_PATH "/sealed/settings.json";
    CHECK(spisor_make_directories(sealed) == 1);
    CHECK(spisor_replace_file(inside, (const uint8_t *)"{}", 2));
    CHECK(chmod(sealed, 0600) == 0);
    spisor_json_file_state state = SPISOR_JSON_FILE_PARSED;
    cJSON *tree = spisor_read_json_file(inside, &state);
    CHECK(chmod(sealed, 0700) == 0);
    cJSON_Delete(tree);
    if (geteuid() != 0) {
        CHECK(state == SPISOR_JSON_FILE_UNREADABLE);
    }
}
#endif

static void read_directory(void) {
    spisor_json_file_state state = SPISOR_JSON_FILE_PARSED;
    cJSON *tree = spisor_read_json_file(SPISOR_SCRATCH_PATH, &state);
    CHECK(tree == NULL);
    CHECK(state == SPISOR_JSON_FILE_UNREADABLE);
}

static void read_malformed_file(void) {
    spisor_json_file_state state = SPISOR_JSON_FILE_PARSED;
    cJSON *tree = read_content("{\"a\":", 5, &state);
    CHECK(tree == NULL);
    CHECK(state == SPISOR_JSON_FILE_UNREADABLE);
}

static void read_empty_file(void) {
    spisor_json_file_state state = SPISOR_JSON_FILE_PARSED;
    cJSON *tree = read_content("", 0, &state);
    CHECK(tree == NULL);
    CHECK(state == SPISOR_JSON_FILE_UNREADABLE);
}

static void read_commented_file(void) {
    static const char content[] = "// note\n{\"a\":1}";
    spisor_json_file_state state = SPISOR_JSON_FILE_ABSENT;
    cJSON *tree = read_content(content, sizeof(content) - 1, &state);
    CHECK(state == SPISOR_JSON_FILE_PARSED);
    cJSON *item = tree == NULL ? NULL : cJSON_GetObjectItem(tree, "a");
    CHECK(item != NULL && item->valuedouble == 1);
    cJSON_Delete(tree);
}

static void read_bom_file(void) {
    static const char content[] = "\xEF\xBB\xBF{}";
    spisor_json_file_state state = SPISOR_JSON_FILE_PARSED;
    cJSON *tree = read_content(content, sizeof(content) - 1, &state);
    CHECK(tree == NULL);
    CHECK(state == SPISOR_JSON_FILE_UNREADABLE);
}

static void read_bom_then_comment_file(void) {
    static const char content[] = "\xEF\xBB\xBF// c\n{}";
    spisor_json_file_state state = SPISOR_JSON_FILE_ABSENT;
    cJSON *tree = read_content(content, sizeof(content) - 1, &state);
    CHECK(state == SPISOR_JSON_FILE_PARSED);
    CHECK(tree != NULL && cJSON_IsObject(tree));
    cJSON_Delete(tree);
}

int main(void) {
    strips_whole_line_comments();
    keeps_a_comment_that_follows_content();
    keeps_slashes_inside_a_string();
    strips_comments_in_a_crlf_text();
    starts_a_comment_line_after_a_lone_cr();
    starts_a_comment_line_after_a_line_separator();
    keeps_a_line_separator_without_a_comment();
    strips_a_comment_led_by_javascript_whitespace();
    strips_blank_lines_up_to_a_comment();
    strips_a_bom_that_leads_a_comment();
    strips_a_comment_holding_a_nul();
    read_absent_file();
    read_beneath_a_file();
    read_where_the_os_will_not_answer();
    read_directory();
    read_malformed_file();
    read_empty_file();
    read_commented_file();
    read_bom_file();
    read_bom_then_comment_file();
    return check_summary("json_file_test");
}
