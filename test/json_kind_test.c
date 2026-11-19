#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "spisor/allocation.h"
#include "spisor/json_kind.h"

static cJSON *parse_json_text(const char *text) {
    return spisor_parse_json_reporting((const uint8_t *)text, strlen(text), &(int){0});
}

static void refuses_text(const char *text) {
    cJSON *document = parse_json_text(text);
    if (document != NULL) {
        fprintf(stderr, "spisor_parse_json_reporting accepted %s\n", text);
        check_failures++;
    }
    cJSON_Delete(document);
}

static void refuses_bytes(const uint8_t *bytes, size_t length) {
    cJSON *document = spisor_parse_json_reporting(bytes, length, &(int){0});
    CHECK(document == NULL);
    cJSON_Delete(document);
}

static void reads_only_the_given_length(void) {
    static const uint8_t bytes[] = {'"', 'a', '"', '!', '!'};
    cJSON *document = spisor_parse_json_reporting(bytes, 3, &(int){0});
    check_text(cJSON_GetStringValue(document), "a", "length-bounded document");
    cJSON_Delete(document);
}

static void accepts_multibyte_utf8(void) {
    cJSON *document = parse_json_text("\"caf\xc3\xa9 \xe6\x97\xa5 \xf0\x9f\x98\x80\"");
    CHECK(cJSON_IsString(document));
    cJSON_Delete(document);
}

static void refuses_invalid_utf8(void) {
    static const uint8_t stray_continuation[] = {'"', 0x80, '"'};
    static const uint8_t lone_ff[] = {'"', 0xFF, '"'};
    static const uint8_t overlong_slash[] = {'"', 0xC0, 0xAF, '"'};
    static const uint8_t surrogate[] = {'"', 0xED, 0xA0, 0x80, '"'};
    static const uint8_t past_unicode[] = {'"', 0xF4, 0x90, 0x80, 0x80, '"'};
    static const uint8_t truncated[] = {'"', 0xE6, 0x97};
    refuses_bytes(stray_continuation, sizeof(stray_continuation));
    refuses_bytes(lone_ff, sizeof(lone_ff));
    refuses_bytes(overlong_slash, sizeof(overlong_slash));
    refuses_bytes(surrogate, sizeof(surrogate));
    refuses_bytes(past_unicode, sizeof(past_unicode));
    refuses_bytes(truncated, sizeof(truncated));
}

static void refuses_invalid_json(void) {
    refuses_text("");
    refuses_text("{");
    refuses_text("1 2");
    refuses_text("{\"a\":}");
}

static char *printed_document(const char *text) {
    cJSON *document = parse_json_text(text);
    char *printed = document == NULL ? NULL : cJSON_PrintUnformatted(document);
    cJSON_Delete(document);
    return printed;
}

static void check_printed(const char *text, const char *expected) {
    char *printed = printed_document(text);
    check_text(printed, expected, text);
    spisor_memory_release(printed);
}

static void refuses_a_nul_in_any_string(void) {
    static const uint8_t raw_nul[] = {'"', 'a', 0x00, 'b', '"'};
    refuses_text("\"a\\u0000b\"");
    refuses_text("{\"a\\u0000\":1}");
    refuses_text("[{\"deep\":[\"C:\\\\x\\u0000y\"]}]");
    refuses_text("{\"argument\":1,\"context\":{\"home\":\"/h\\u0000\"}}");
    refuses_bytes(raw_nul, sizeof(raw_nul));
    check_printed("\"\\\\u0000\"", "\"\\\\u0000\"");
}

static void keeps_the_last_of_a_duplicated_key_everywhere(void) {
    check_printed("{\"id\":\"a\",\"id\":\"b\"}", "{\"id\":\"b\"}");
    check_printed("{\"x\":1,\"y\":2,\"x\":3}", "{\"x\":3,\"y\":2}");
    check_printed("{\"x\":1,\"y\":2,\"x\":3,\"x\":4}", "{\"x\":4,\"y\":2}");
    check_printed("[{\"o\":{\"k\":1,\"k\":{\"m\":1,\"m\":2}}}]", "[{\"o\":{\"k\":{\"m\":2}}}]");
    check_printed("{\"K\":1,\"k\":2}", "{\"K\":1,\"k\":2}");
}

static void decodes_an_unpaired_surrogate_as_a_replacement_character(void) {
    check_printed("\"\\ud800\"", "\"\xef\xbf\xbd\"");
    check_printed("\"a\\uDC00b\"", "\"a\xef\xbf\xbd" "b\"");
    check_printed("\"\\ud800\\ud800\\udc00\"", "\"\xef\xbf\xbd\xf0\x90\x80\x80\"");
    check_printed("\"\\ud83d\\ude00\"", "\"\xf0\x9f\x98\x80\"");
    check_printed("\"\\ud800x\"", "\"\xef\xbf\xbdx\"");
    check_printed("{\"\\udfff\":\"\\ud800\\\\ud800\"}", "{\"\xef\xbf\xbd\":\"\xef\xbf\xbd\\\\ud800\"}");
}

static void parses_a_whole_document_as_json_parse_does(void) {
    static const uint8_t invalid_utf8[] = {'"', 0xFF, '"'};
    cJSON *document = parse_json_text(" {\"k\":1,\"k\":\"\\ud800\"} ");
    check_text(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(document, "k")), "\xef\xbf\xbd", "parse_json");
    cJSON_Delete(document);
    CHECK(spisor_parse_json_reporting(invalid_utf8, sizeof(invalid_utf8), &(int){0}) == NULL);
    CHECK(parse_json_text("1 2") == NULL);
    CHECK(parse_json_text("\"a\\u0000\"") == NULL);
    CHECK(parse_json_text("") == NULL);
}

static const char *const refused_by_json_parse[] = {
    "\xEF\xBB\xBF{}", "\v{}", "{\f}", "{\x01}", "[\xC2\xA0" "1]", "\"a\tb\"", "\"a\x1f" "b\"",
    "01", "-01", "1.", "-", "1e", "1e+", ".5", "+1", "[1,]", "{\"a\":1,}", "\"\\'\"",
    "\"\\u12\"", "\"\\x41\"", "tru", "nul", "[1 2]", "{\"a\" 1}", "{a:1}", "\"open",
};

static const char *const accepted_by_json_parse[] = {
    " \t\r\n{} \t\r\n", "-0", "0.25", "1.5E+3", "1e-2", "-12.5e10", "\"\\\"\\\\\\/\\b\\f\\n\\r\\t\\u00e9\"",
    "\"a\xE2\x80\xA8z\"", "[true,false,null,[],{}]", "{\"\":0}",
};

static void parses_only_what_json_parse_accepts(void) {
    for (size_t index = 0; index < sizeof(refused_by_json_parse) / sizeof(refused_by_json_parse[0]); index++) {
        cJSON *document = parse_json_text(refused_by_json_parse[index]);
        if (document != NULL) {
            fprintf(stderr, "spisor_parse_json_reporting accepted %s\n", refused_by_json_parse[index]);
            check_failures++;
        }
        cJSON_Delete(document);
    }
    for (size_t index = 0; index < sizeof(accepted_by_json_parse) / sizeof(accepted_by_json_parse[0]); index++) {
        cJSON *document = parse_json_text(accepted_by_json_parse[index]);
        if (document == NULL) {
            fprintf(stderr, "spisor_parse_json_reporting refused %s\n", accepted_by_json_parse[index]);
            check_failures++;
        }
        cJSON_Delete(document);
    }
    refuses_text("\xEF\xBB\xBF{\"argument\":{},\"context\":{}}");
}

int main(void) {
    reads_only_the_given_length();
    accepts_multibyte_utf8();
    refuses_invalid_utf8();
    refuses_invalid_json();
    refuses_a_nul_in_any_string();
    keeps_the_last_of_a_duplicated_key_everywhere();
    decodes_an_unpaired_surrogate_as_a_replacement_character();
    parses_a_whole_document_as_json_parse_does();
    parses_only_what_json_parse_accepts();
    return check_summary("json_kind_test");
}
