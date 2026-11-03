#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#define CURRENT_PROCESS_ID _getpid()
#else
#include <unistd.h>
#define CURRENT_PROCESS_ID getpid()
#endif

#include "change_kinds.h"
#include "check.h"
#include "scratch_directory.h"
#include "spisor/file.h"

/** A json-key change applied as runtime applies it, in its two steps, so a test can act between them. */
typedef struct two_step {
    char file[1200];
    cJSON *change;
    cJSON *record;
} two_step;

static void fresh_file(two_step *step, const char *label, const char *text) {
    char directory[1024];
    snprintf(directory, sizeof directory, RUNTIME_SCRATCH_PATH "/json-key-%d-%s", (int)CURRENT_PROCESS_ID, label);
    start_empty_directory(directory);
    snprintf(step->file, sizeof step->file, "%s/settings.json", directory);
    CHECK(spisor_replace_file(step->file, (const uint8_t *)text, strlen(text)) == 1);
}

static void write_file(const two_step *step, const char *text) {
    CHECK(spisor_replace_file(step->file, (const uint8_t *)text, strlen(text)) == 1);
}

static void file_is(const two_step *step, const char *expected, const char *label) {
    size_t length = 0;
    char *actual = (char *)spisor_read_file(step->file, &length);
    check_text(actual, expected, label);
    free(actual);
}

static void prepare(two_step *step, const char *key_json, const char *value_json) {
    step->change = cJSON_CreateObject();
    cJSON_AddStringToObject(step->change, "kind", "json-key");
    cJSON_AddStringToObject(step->change, "file", step->file);
    cJSON_AddItemToObject(step->change, "key", cJSON_Parse(key_json));
    cJSON_AddItemToObject(step->change, "value", cJSON_Parse(value_json));
    runtime_change_note note = {""};
    CHECK(runtime_change_prepare(step->change, NULL, &step->record, &note) == RUNTIME_CHANGE_DONE);
    cJSON_AddStringToObject(step->record, "kind", "json-key");
}

static void perform(two_step *step) {
    runtime_change_note note = {""};
    CHECK(step->record != NULL && runtime_change_perform(step->change, step->record, &note) == RUNTIME_CHANGE_DONE);
}

static void reverse(two_step *step, runtime_change_reversal expected) {
    runtime_change_note note = {""};
    CHECK(runtime_change_reverse(step->record, 0, &note) == expected);
    cJSON_Delete(step->change);
    cJSON_Delete(step->record);
}

static const char *const original = "{\n  \"a\": 1.50\n}\n";

static void a_member_added_between_the_reads_survives_the_reverse(void) {
    two_step step;
    fresh_file(&step, "between-reads", original);
    prepare(&step, "[\"b\"]", "2");
    const char *changed = "{\n  \"a\": 1.50,\n  \"c\": \"\\u00e9\"\n}\n";
    write_file(&step, changed);
    perform(&step);
    file_is(&step, "{\n  \"a\": 1.50,\n  \"c\": \"\\u00e9\",\n  \"b\": 2\n}\n", "edited the bytes perform read");
    reverse(&step, RUNTIME_CHANGE_REVERSED);
    file_is(&step, changed, "reversed to the bytes really edited");
}

static void a_key_that_appeared_between_the_reads_is_restored_not_deleted(void) {
    two_step step;
    fresh_file(&step, "appeared", original);
    prepare(&step, "[\"b\"]", "2");
    const char *changed = "{\n  \"a\": 1.50,\n  \"b\": 5.0\n}\n";
    write_file(&step, changed);
    perform(&step);
    file_is(&step, "{\n  \"a\": 1.50,\n  \"b\": 2\n}\n", "replaced the key that appeared");
    reverse(&step, RUNTIME_CHANGE_REVERSED);
    file_is(&step, changed, "the appeared key comes back as it was");
}

static void the_record_holds_none_of_the_users_bytes(void) {
    two_step step;
    const char *secret_file = "{\n  \"oauthAccount\": {\"token\": \"SECRET-TOKEN-0123456789\"},\n  \"theme\": \"dark\"\n}\n";
    fresh_file(&step, "no-copy", secret_file);
    prepare(&step, "[\"theme\"]", "\"light\"");
    perform(&step);
    char *record = cJSON_PrintUnformatted(step.record);
    CHECK(record != NULL && strstr(record, "SECRET-TOKEN") == NULL && strstr(record, "oauthAccount") == NULL);
    free(record);
    reverse(&step, RUNTIME_CHANGE_REVERSED);
    file_is(&step, secret_file, "exact reverse from the inverse splice");
}

static void created_objects_the_user_emptied_go_on_reverse(void) {
    two_step step;
    fresh_file(&step, "emptied-created", original);
    prepare(&step, "[\"m\",\"n\",\"o\"]", "1");
    perform(&step);
    write_file(&step, "{\n  \"a\": 1.50,\n  \"m\": {\n    \"n\": {}\n  }\n}\n");
    reverse(&step, RUNTIME_CHANGE_REVERSED);
    file_is(&step, original, "the created objects are gone with the leaf");
}

static void a_file_with_a_bom_and_a_comment_line_is_edited_and_restored(void) {
    two_step step;
    const char *marked = "\xEF\xBB\xBF// note\n{\n  \"a\": 1E+5\n}\n";
    fresh_file(&step, "bom", marked);
    prepare(&step, "[\"b\"]", "true");
    perform(&step);
    file_is(&step, "\xEF\xBB\xBF// note\n{\n  \"a\": 1E+5,\n  \"b\": true\n}\n", "BOM kept");
    reverse(&step, RUNTIME_CHANGE_REVERSED);
    file_is(&step, marked, "BOM file restored");
}

static void an_inverse_that_misses_the_pre_image_is_not_trusted(void) {
    two_step step;
    fresh_file(&step, "untrusted-inverse", original);
    prepare(&step, "[\"b\"]", "2");
    perform(&step);
    cJSON_ReplaceItemInObjectCaseSensitive(step.record, "spliceAt", cJSON_CreateNumber(1));
    reverse(&step, RUNTIME_CHANGE_REVERSED);
    file_is(&step, original, "the member is removed in place instead");
}

int main(void) {
    an_inverse_that_misses_the_pre_image_is_not_trusted();
    a_file_with_a_bom_and_a_comment_line_is_edited_and_restored();
    a_member_added_between_the_reads_survives_the_reverse();
    a_key_that_appeared_between_the_reads_is_restored_not_deleted();
    the_record_holds_none_of_the_users_bytes();
    created_objects_the_user_emptied_go_on_reverse();
    return check_summary("json_key_change_test");
}
