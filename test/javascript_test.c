#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "spisor/javascript.h"

static void check_decoded(const char *bytes, const char *expected, const char *label) {
    size_t length = 0;
    uint8_t *decoded = spisor_decode_utf8_lossy((const uint8_t *)bytes, strlen(bytes), &length);
    CHECK(decoded != NULL && length == strlen(expected));
    check_text((const char *)decoded, expected, label);
    free(decoded);
}

#define FFFD "\xEF\xBF\xBD"

static void decodes_utf8_as_node_does(void) {
    check_decoded("plain \xC3\xA9 \xF0\x9F\x98\x80", "plain \xC3\xA9 \xF0\x9F\x98\x80", "well-formed text is kept");
    check_decoded("a\xC3(b", "a" FFFD "(b", "a truncated two-byte sequence leaves its ending byte");
    check_decoded("\xE0\x80z", FFFD FFFD "z", "an overlong lead and its continuation");
    check_decoded("\xED\xA0\x80", FFFD FFFD FFFD, "an encoded surrogate");
    check_decoded("\xF0\x9F\x98", FFFD, "a truncated four-byte sequence at the end");
    check_decoded("\xFF\xFE", FFFD FFFD, "bytes that never lead");
    check_decoded("\xF4\x90\x80\x80", FFFD FFFD FFFD FFFD, "past U+10FFFF");
    check_decoded("\xE2\x82x", FFFD "x", "a truncated three-byte sequence");
    check_decoded("", "", "nothing");
}

static void decodes_a_nul_and_stops_only_at_the_length(void) {
    static const uint8_t bytes[] = {'a', 0, 0xFF};
    size_t length = 0;
    uint8_t *decoded = spisor_decode_utf8_lossy(bytes, sizeof(bytes), &length);
    CHECK(decoded != NULL && length == 5 && decoded[0] == 'a' && decoded[1] == 0 && memcmp(decoded + 2, FFFD, 3) == 0);
    free(decoded);
}

static void walks_code_points(void) {
    const char *text = "a\xC3\xA9\xE2\x80\xA8\xF0\x9F\x98\x80";
    size_t index = 0;
    CHECK(spisor_utf8_next(text, &index) == 'a' && index == 1);
    CHECK(spisor_utf8_next(text, &index) == 0xE9 && index == 3);
    CHECK(spisor_utf8_next(text, &index) == 0x2028 && index == 6);
    CHECK(spisor_utf8_next(text, &index) == 0x1F600 && index == 10);
}

static void knows_javascript_whitespace(void) {
    static const uint32_t spaces[] = {0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x20, 0xA0, 0x1680, 0x2000,
                                      0x200A, 0x2028, 0x2029, 0x202F, 0x205F, 0x3000, 0xFEFF};
    static const uint32_t others[] = {0x00, 0x08, 0x0E, 0x1F, 0x21, 0x85, 0x180E, 0x200B, 0x2060, 0xFFFD};
    for (size_t index = 0; index < sizeof(spaces) / sizeof(spaces[0]); index++) {
        CHECK(spisor_is_javascript_space(spaces[index]));
    }
    for (size_t index = 0; index < sizeof(others) / sizeof(others[0]); index++) {
        CHECK(!spisor_is_javascript_space(others[index]));
    }
}

static void check_trimmed(const char *text, const char *expected) {
    char *trimmed = spisor_javascript_trim(text);
    check_text(trimmed, expected, text);
    free(trimmed);
}

static void trims_as_string_prototype_trim_does(void) {
    check_trimmed("  a b \t\n", "a b");
    check_trimmed("\xC2\xA0\xEF\xBB\xBF" "x" "\xE2\x80\x83\xE3\x80\x80", "x");
    check_trimmed("\xE2\x80\x8B" "x", "\xE2\x80\x8B" "x");
    check_trimmed(" \xE2\x80\xA8 ", "");
    check_trimmed("", "");
    check_trimmed("\xC3\xA9", "\xC3\xA9");
}

static void check_string(const char *json, const char *expected) {
    cJSON *value = cJSON_Parse(json);
    char *string = spisor_javascript_string(value);
    check_text(string, expected, json);
    free(string);
    cJSON_Delete(value);
}

static void converts_values_as_string_does(void) {
    check_string("0", "0");
    check_string("-0", "0");
    check_string("5", "5");
    check_string("-5", "-5");
    check_string("1.5e-7", "1.5e-7");
    check_string("1e21", "1e+21");
    check_string("1e20", "100000000000000000000");
    check_string("123.456", "123.456");
    check_string("0.000001", "0.000001");
    check_string("1e-7", "1e-7");
    check_string("1.7976931348623157e308", "1.7976931348623157e+308");
    check_string("5e-324", "5e-324");
    check_string("0.1", "0.1");
    check_string("-2.5e-10", "-2.5e-10");
    check_string("123456789012345680000", "123456789012345680000");
    check_string("0.3333333333333333", "0.3333333333333333");
    check_string("1e400", "Infinity");
    check_string("true", "true");
    check_string("false", "false");
    check_string("null", "null");
    check_string("\"text\"", "text");
    check_string("[\"P\",null,[1,\"Q\"],true]", "P,,1,Q,true");
    check_string("[]", "");
    check_string("{\"a\":1}", "[object Object]");
}

int main(void) {
    decodes_utf8_as_node_does();
    decodes_a_nul_and_stops_only_at_the_length();
    walks_code_points();
    knows_javascript_whitespace();
    trims_as_string_prototype_trim_does();
    converts_values_as_string_does();
    return check_summary("javascript_test");
}
