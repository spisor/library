#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "spisor/json_kind.h"

typedef struct parsed_failure {
    int32_t status;
    cJSON *object;
} parsed_failure;

static int32_t parse_text(const char *text, int contextual, spisor_call *call, uint8_t **out, size_t *out_len) {
    return spisor_parse_call((const uint8_t *)text, strlen(text), contextual, call, out, out_len);
}

static parsed_failure refuse(const uint8_t *in, size_t in_len, int contextual) {
    spisor_call call = {NULL, NULL};
    uint8_t *out = NULL;
    size_t out_len = 0;
    parsed_failure failure;
    failure.status = spisor_parse_call(in, in_len, contextual, &call, &out, &out_len);
    failure.object = out == NULL ? NULL : cJSON_ParseWithLength((const char *)out, out_len);
    spisor_release(out);
    spisor_discard_call(&call);
    return failure;
}

static int is_failure_object(const cJSON *object) {
    return cJSON_IsObject(object) && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(object, "message")) &&
           cJSON_IsString(cJSON_GetObjectItemCaseSensitive(object, "fix"));
}

static int failure_names(const cJSON *object, const char *word) {
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(object, "message");
    return cJSON_IsString(message) && strstr(message->valuestring, word) != NULL;
}

static void refuses_text(const char *text, int contextual, const char *named) {
    parsed_failure failure = refuse((const uint8_t *)text, strlen(text), contextual);
    CHECK(failure.status == SPISOR_STATUS_FAILURE);
    CHECK(is_failure_object(failure.object));
    if (named != NULL && !failure_names(failure.object, named)) {
        fprintf(stderr, "refusal of %s does not name %s\n", text, named);
        check_failures++;
    }
    cJSON_Delete(failure.object);
}

static void refuses_bytes(const uint8_t *bytes, size_t length) {
    parsed_failure failure = refuse(bytes, length, 0);
    CHECK(failure.status == SPISOR_STATUS_FAILURE);
    CHECK(is_failure_object(failure.object));
    CHECK(failure_names(failure.object, "UTF-8"));
    cJSON_Delete(failure.object);
}

static void splits_an_envelope(void) {
    spisor_call call = {NULL, NULL};
    uint8_t *out = NULL;
    size_t out_len = 0;
    const char *envelope =
        "{\"argument\":{\"id\":\"alpha\"},\"context\":{\"environment\":{\"HUB\":\"one\",\"HUB\":\"two\"},"
        "\"home\":\"/home/me\",\"arguments\":[\"node\",\"cli\"]}}";
    CHECK(parse_text(envelope, 1, &call, &out, &out_len) == SPISOR_STATUS_SUCCESS);
    CHECK(out == NULL);
    CHECK(cJSON_IsObject(call.argument));
    check_text(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(call.argument, "id")), "alpha", "argument id");
    check_text(spisor_context_string(&call, "home"), "/home/me", "home fact");
    check_text(spisor_environment(&call, "HUB"), "two", "last duplicated environment entry");
    CHECK(spisor_environment(&call, "hub") == NULL);
    CHECK(spisor_environment(&call, "ABSENT") == NULL);
    CHECK(spisor_context_string(&call, "arguments") == NULL);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(call.context, "arguments")) == 2);
    spisor_discard_call(&call);
    CHECK(call.argument == NULL && call.context == NULL);
}

static void accepts_a_context_without_facts(void) {
    spisor_call call = {NULL, NULL};
    uint8_t *out = NULL;
    size_t out_len = 0;
    CHECK(parse_text("{\"context\":{},\"argument\":null}", 1, &call, &out, &out_len) == SPISOR_STATUS_SUCCESS);
    CHECK(cJSON_IsNull(call.argument));
    CHECK(spisor_context_string(&call, "home") == NULL);
    CHECK(spisor_environment(&call, "HOME") == NULL);
    spisor_discard_call(&call);
}

static void keeps_a_plain_input_as_the_argument(void) {
    spisor_call call = {NULL, NULL};
    uint8_t *out = NULL;
    size_t out_len = 0;
    CHECK(parse_text(" {\"argument\":[1,2]} ", 0, &call, &out, &out_len) == SPISOR_STATUS_SUCCESS);
    CHECK(cJSON_IsObject(call.argument));
    CHECK(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(call.argument, "argument")));
    CHECK(call.context == NULL);
    CHECK(spisor_environment(&call, "HOME") == NULL);
    spisor_discard_call(&call);
}

static void reads_only_the_given_length(void) {
    spisor_call call = {NULL, NULL};
    uint8_t *out = NULL;
    size_t out_len = 0;
    static const uint8_t bytes[] = {'"', 'a', '"', '!', '!'};
    CHECK(spisor_parse_call(bytes, 3, 0, &call, &out, &out_len) == SPISOR_STATUS_SUCCESS);
    check_text(cJSON_GetStringValue(call.argument), "a", "length-bounded argument");
    spisor_discard_call(&call);
}

static void accepts_multibyte_utf8(void) {
    spisor_call call = {NULL, NULL};
    uint8_t *out = NULL;
    size_t out_len = 0;
    CHECK(parse_text("\"caf\xc3\xa9 \xe6\x97\xa5 \xf0\x9f\x98\x80\"", 0, &call, &out, &out_len) == SPISOR_STATUS_SUCCESS);
    spisor_discard_call(&call);
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
    refuses_text("", 0, "JSON");
    refuses_text("{", 0, "JSON");
    refuses_text("1 2", 0, "JSON");
    refuses_text("{\"a\":}", 1, "JSON");
}

static void refuses_a_malformed_envelope(void) {
    refuses_text("[1]", 1, "envelope");
    refuses_text("{\"context\":{}}", 1, "argument");
    refuses_text("{\"argument\":1}", 1, "context");
    refuses_text("{\"argument\":1,\"context\":[]}", 1, "context");
    refuses_text("{\"argument\":1,\"context\":{\"environment\":[]}}", 1, "environment");
    refuses_text("{\"argument\":1,\"context\":{\"environment\":{\"A\":1}}}", 1, "environment");
    refuses_text("{\"argument\":1,\"context\":{\"home\":7}}", 1, "home");
    refuses_text("{\"argument\":1,\"context\":{\"arguments\":\"node\"}}", 1, "arguments");
    refuses_text("{\"argument\":1,\"context\":{\"arguments\":[\"node\",2]}}", 1, "arguments");
    refuses_text("{\"argument\":1,\"context\":{\"cwd\":\"/\"}}", 1, "cwd");
}

static void escapes_a_failure(void) {
    uint8_t *out = NULL;
    size_t out_len = 0;
    const char *message = "say \"hi\"\\ now\n\t\x01";
    CHECK(spisor_fail(message, "fix \"it\"", &out, &out_len) == SPISOR_STATUS_FAILURE);
    CHECK(out != NULL && out_len == strlen((const char *)out));
    cJSON *object = cJSON_ParseWithLength((const char *)out, out_len);
    CHECK(is_failure_object(object));
    check_text(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, "message")), message, "message round trip");
    check_text(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, "fix")), "fix \"it\"", "fix round trip");
    CHECK(strstr((const char *)out, "\\n") != NULL);
    CHECK(strstr((const char *)out, "\\u0001") != NULL);
    cJSON_Delete(object);
    spisor_release(out);
}

static void writes_a_result(void) {
    uint8_t *out = NULL;
    size_t out_len = 0;
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "id", "alpha");
    CHECK(spisor_succeed(result, &out, &out_len) == SPISOR_STATUS_SUCCESS);
    CHECK(out_len == strlen("{\"id\":\"alpha\"}"));
    CHECK(out != NULL && memcmp(out, "{\"id\":\"alpha\"}", out_len) == 0);
    cJSON_Delete(result);
    spisor_release(out);
}

static char *printed_argument(const char *text) {
    spisor_call call = {NULL, NULL};
    uint8_t *out = NULL;
    size_t out_len = 0;
    char *printed = NULL;
    if (parse_text(text, 0, &call, &out, &out_len) == SPISOR_STATUS_SUCCESS) {
        printed = cJSON_PrintUnformatted(call.argument);
    }
    spisor_release(out);
    spisor_discard_call(&call);
    return printed;
}

static void check_printed(const char *text, const char *expected) {
    char *printed = printed_argument(text);
    check_text(printed, expected, text);
    free(printed);
}

static void refuses_a_nul_in_any_string(void) {
    refuses_text("\"a\\u0000b\"", 0, "NUL");
    refuses_text("{\"a\\u0000\":1}", 0, "NUL");
    refuses_text("[{\"deep\":[\"C:\\\\x\\u0000y\"]}]", 0, "NUL");
    refuses_text("{\"argument\":1,\"context\":{\"home\":\"/h\\u0000\"}}", 1, "NUL");
    static const uint8_t raw_nul[] = {'"', 'a', 0x00, 'b', '"'};
    parsed_failure failure = refuse(raw_nul, sizeof(raw_nul), 0);
    CHECK(failure.status == SPISOR_STATUS_FAILURE);
    CHECK(is_failure_object(failure.object));
    cJSON_Delete(failure.object);
    check_printed("\"\\\\u0000\"", "\"\\\\u0000\"");
}

static void keeps_the_last_of_a_duplicated_key_everywhere(void) {
    check_printed("{\"id\":\"a\",\"id\":\"b\"}", "{\"id\":\"b\"}");
    check_printed("{\"x\":1,\"y\":2,\"x\":3}", "{\"x\":3,\"y\":2}");
    check_printed("{\"x\":1,\"y\":2,\"x\":3,\"x\":4}", "{\"x\":4,\"y\":2}");
    check_printed("[{\"o\":{\"k\":1,\"k\":{\"m\":1,\"m\":2}}}]", "[{\"o\":{\"k\":{\"m\":2}}}]");
    check_printed("{\"K\":1,\"k\":2}", "{\"K\":1,\"k\":2}");

    spisor_call call = {NULL, NULL};
    uint8_t *out = NULL;
    size_t out_len = 0;
    const char *envelope = "{\"argument\":1,\"argument\":{\"id\":\"b\"},"
                           "\"context\":{\"environment\":{\"A\":1,\"A\":\"x\"},\"home\":2,\"home\":\"/h\"}}";
    CHECK(parse_text(envelope, 1, &call, &out, &out_len) == SPISOR_STATUS_SUCCESS);
    check_text(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(call.argument, "id")), "b", "last argument");
    check_text(spisor_environment(&call, "A"), "x", "last environment entry over a malformed first");
    check_text(spisor_context_string(&call, "home"), "/h", "last home over a malformed first");
    spisor_release(out);
    spisor_discard_call(&call);
}

static void decodes_an_unpaired_surrogate_as_a_replacement_character(void) {
    check_printed("\"\\ud800\"", "\"\xef\xbf\xbd\"");
    check_printed("\"a\\uDC00b\"", "\"a\xef\xbf\xbd" "b\"");
    check_printed("\"\\ud800\\ud800\\udc00\"", "\"\xef\xbf\xbd\xf0\x90\x80\x80\"");
    check_printed("\"\\ud83d\\ude00\"", "\"\xf0\x9f\x98\x80\"");
    check_printed("\"\\ud800x\"", "\"\xef\xbf\xbdx\"");
    check_printed("{\"\\udfff\":\"\\ud800\\\\ud800\"}", "{\"\xef\xbf\xbd\":\"\xef\xbf\xbd\\\\ud800\"}");
}

static cJSON *parse_json_text(const char *text) {
    return spisor_parse_json((const uint8_t *)text, strlen(text));
}

static void parses_a_whole_document_as_json_parse_does(void) {
    static const uint8_t invalid_utf8[] = {'"', 0xFF, '"'};
    cJSON *document = parse_json_text(" {\"k\":1,\"k\":\"\\ud800\"} ");
    check_text(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(document, "k")), "\xef\xbf\xbd", "parse_json");
    cJSON_Delete(document);
    CHECK(spisor_parse_json(invalid_utf8, sizeof(invalid_utf8)) == NULL);
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
            fprintf(stderr, "spisor_parse_json accepted %s\n", refused_by_json_parse[index]);
            check_failures++;
        }
        cJSON_Delete(document);
        refuses_text(refused_by_json_parse[index], 0, "JSON");
    }
    for (size_t index = 0; index < sizeof(accepted_by_json_parse) / sizeof(accepted_by_json_parse[0]); index++) {
        cJSON *document = parse_json_text(accepted_by_json_parse[index]);
        if (document == NULL) {
            fprintf(stderr, "spisor_parse_json refused %s\n", accepted_by_json_parse[index]);
            check_failures++;
        }
        cJSON_Delete(document);
    }
    refuses_text("\xEF\xBB\xBF{\"argument\":{},\"context\":{}}", 1, "JSON");
}

static void refuses_to_succeed_without_a_result(void) {
    uint8_t *out = NULL;
    size_t out_len = 0;
    CHECK(spisor_succeed(NULL, &out, &out_len) == SPISOR_STATUS_FAILURE);
    cJSON *object = out == NULL ? NULL : cJSON_ParseWithLength((const char *)out, out_len);
    CHECK(is_failure_object(object));
    CHECK(failure_names(object, "no result"));
    cJSON_Delete(object);
    spisor_release(out);
}

int main(void) {
    splits_an_envelope();
    accepts_a_context_without_facts();
    keeps_a_plain_input_as_the_argument();
    reads_only_the_given_length();
    accepts_multibyte_utf8();
    refuses_invalid_utf8();
    refuses_invalid_json();
    refuses_a_malformed_envelope();
    escapes_a_failure();
    writes_a_result();
    refuses_a_nul_in_any_string();
    keeps_the_last_of_a_duplicated_key_everywhere();
    decodes_an_unpaired_surrogate_as_a_replacement_character();
    refuses_to_succeed_without_a_result();
    parses_a_whole_document_as_json_parse_does();
    parses_only_what_json_parse_accepts();
    return check_summary("json_kind_test");
}
