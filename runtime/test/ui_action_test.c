#include <stdio.h>
#include <string.h>

#include "definition_fixture.h"
#include "ui_fixture.h"

#define ECHO_ACTIONS                                                                                                 \
    "{\"id\": \"viewer\", \"actions\": {"                                                                          \
    "\"echo\": {\"label\": \"Echo\", \"call\": {\"service\": \"viewer:status\", \"method\": \"echo\","             \
    " \"argument\": {\"item\": \"declared\", \"keep\": 1}}},"                                                      \
    "\"bare\": {\"label\": \"Bare\", \"call\": {\"service\": \"viewer:status\", \"method\": \"echo\","             \
    " \"argument\": \"oops\"}},"                                                                                   \
    "\"uncalled\": {\"label\": \"Uncalled\", \"call\": {\"service\": \"viewer:status\"}},"                         \
    "\"loose\": \"restart\"}}"

/** An action request over `manifest`, NULL for the viewer fixture's, running `action` with `row` laid in. */
static cJSON *action_request(const char *manifest, const char *action, const char *row) {
    cJSON *request = row == NULL ? cJSON_CreateObject() : parsed(row);
    cJSON_AddItemToObject(request, "description", manifest == NULL ? fixture_manifest("viewer") : parsed(manifest));
    cJSON_AddStringToObject(request, "action", action);
    return request;
}

static void check_action(const char *manifest, const char *action, const char *row, const char *arguments,
                         const char *label) {
    cJSON *call = successful(UI_ACTION, action_request(manifest, action, row), label);
    check_text(text_at(call, "plugin"), "viewer", label);
    check_text(text_at(call, "service"), "viewer:status", label);
    check_text(text_at(call, "method"), "echo", label);
    check_json_at(call, "arguments", arguments, label);
    cJSON_Delete(call);
}

static void calls_the_actions_binding_with_the_rows_item_merged_over_its_declared_argument(void) {
    check_action(NULL, "restart", "{\"item\": \"x\"}", "{\"force\": true, \"item\": \"x\"}", "a row's item");
}

static void calls_the_actions_binding_with_its_declared_argument_alone(void) {
    check_action(NULL, "restart", NULL, "{\"force\": true, \"item\": \"declared\"}", "the declared argument");
}

static void refuses_an_action_the_plugin_does_not_declare(void) {
    expect_ui_refusal(ui_call(UI_ACTION, action_request(NULL, "explode", NULL)), SPISOR_REFUSED,
                      "viewer declares no action explode", "an undeclared action");
    expect_ui_refusal(ui_call(UI_ACTION, action_request(ECHO_ACTIONS, "loose", NULL)), SPISOR_REFUSED,
                      "viewer declares no action loose", "an action declared as no object");
    expect_ui_refusal(ui_call(UI_ACTION, action_request("{\"id\": \"viewer\"}", "restart", NULL)), SPISOR_REFUSED,
                      "viewer declares no action restart", "a manifest declaring no actions");
}

static void merges_the_rows_item_over_the_declared_one(void) {
    check_action(ECHO_ACTIONS, "echo", "{\"item\": \"x\"}", "{\"item\": \"x\", \"keep\": 1}", "the row over declared");
}

static void lets_a_row_set_only_item_and_items_never_another_declared_key(void) {
    check_action(ECHO_ACTIONS, "echo", "{\"item\": \"x\", \"items\": [\"x\", \"y\"]}",
                 "{\"item\": \"x\", \"items\": [\"x\", \"y\"], \"keep\": 1}", "item and items alone");
    check_action(ECHO_ACTIONS, "echo", "{\"items\": []}", "{\"item\": \"declared\", \"keep\": 1, \"items\": []}",
                 "an empty selection is still a selection");
}

static void lays_the_input_over_the_declared_argument(void) {
    check_action(NULL, "restart", "{\"input\": {\"force\": false, \"note\": \"n\"}}",
                 "{\"force\": false, \"item\": \"declared\", \"note\": \"n\"}", "the input over declared");
    check_action(NULL, "restart", "{\"input\": {\"item\": \"typed\"}}", "{\"force\": true, \"item\": \"typed\"}",
                 "the input's item with no row");
}

static void keeps_the_rows_item_and_items_over_the_input(void) {
    check_action(NULL, "restart",
                 "{\"input\": {\"item\": \"typed\", \"items\": [\"typed\"]}, \"item\": \"x\", \"items\": []}",
                 "{\"force\": true, \"item\": \"x\", \"items\": []}", "the row over the input");
}

static void keeps_each_replaced_member_where_the_declared_argument_put_it(void) {
    const char *row = "{\"input\": {\"extra\": 2, \"keep\": 3}, \"item\": \"x\"}";
    cJSON *call = successful(UI_ACTION, action_request(ECHO_ACTIONS, "echo", row), "the order of members");
    char *printed = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(call, "arguments"));
    check_text(printed, "{\"item\":\"x\",\"keep\":3,\"extra\":2}", "the order of members");
    cJSON_free(printed);
    cJSON_Delete(call);
}

static void reads_a_declared_argument_that_is_not_a_record_as_none(void) {
    check_action(ECHO_ACTIONS, "bare", "{\"item\": \"x\"}", "{\"item\": \"x\"}", "a declared text argument");
}

static void refuses_an_action_whose_call_names_no_service_or_method(void) {
    expect_ui_refusal(ui_call(UI_ACTION, action_request(ECHO_ACTIONS, "uncalled", NULL)), SPISOR_REFUSED,
                      "viewer declares action uncalled with no call naming a service and a method", "no method");
}

static void refuses_an_argument_out_of_its_shape(void) {
    expect_ui_refusal(ui_call(UI_ACTION, action_request("{\"label\": \"no id\"}", "restart", NULL)), SPISOR_MALFORMED,
                      "description", "a description with no id");
    expect_ui_refusal(ui_call(UI_ACTION, action_request(NULL, "restart", "{\"input\": [\"x\"]}")), SPISOR_MALFORMED,
                      "input", "an input that is no record");
    cJSON *request = action_request(NULL, "restart", NULL);
    cJSON_DeleteItemFromObjectCaseSensitive(request, "action");
    expect_ui_refusal(ui_call(UI_ACTION, request), SPISOR_MALFORMED, "action", "no action");
}

static spisor_status action_with_items(uint32_t has_items, const char *const *items, size_t item_count) {
    const runtime_ui_definition *ui = runtime_ui_definition_of();
    spisor_value_tree description = SPISOR_INIT(spisor_value_tree);
    runtime_ui_action_argument argument = SPISOR_INIT(runtime_ui_action_argument);
    runtime_ui_action_answer answer = SPISOR_INIT(runtime_ui_action_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    CHECK(value_of_text("{\"id\": \"viewer\"}", &description));
    argument.description = &description.root;
    argument.action = "restart";
    argument.has_items = has_items;
    argument.items = items;
    argument.item_count = item_count;
    status = ui->action(ui->context, &argument, &answer, &refusal);
    ui->release_action_answer(ui->context, &answer);
    ui->release_refusal(ui->context, &refusal);
    spisor_value_release(&description);
    return status;
}

static void refuses_items_out_of_their_shape_and_ignores_them_when_not_given(void) {
    const char *const gap[] = {"x", NULL};
    CHECK(action_with_items(2, NULL, 0) == SPISOR_MALFORMED);
    CHECK(action_with_items(1, NULL, 1) == SPISOR_MALFORMED);
    CHECK(action_with_items(1, gap, 2) == SPISOR_MALFORMED);
    CHECK(action_with_items(0, NULL, 1) == SPISOR_REFUSED);
}

int main(void) {
    calls_the_actions_binding_with_the_rows_item_merged_over_its_declared_argument();
    calls_the_actions_binding_with_its_declared_argument_alone();
    refuses_an_action_the_plugin_does_not_declare();
    merges_the_rows_item_over_the_declared_one();
    lets_a_row_set_only_item_and_items_never_another_declared_key();
    lays_the_input_over_the_declared_argument();
    keeps_the_rows_item_and_items_over_the_input();
    keeps_each_replaced_member_where_the_declared_argument_put_it();
    reads_a_declared_argument_that_is_not_a_record_as_none();
    refuses_an_action_whose_call_names_no_service_or_method();
    refuses_an_argument_out_of_its_shape();
    refuses_items_out_of_their_shape_and_ignores_them_when_not_given();
    return check_summary("ui_action_test");
}
