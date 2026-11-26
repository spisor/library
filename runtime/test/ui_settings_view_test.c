#include <stdio.h>
#include <string.h>

#include "ui_fixture.h"

static cJSON *settings_view_of(const char *manifest) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddItemToObject(request, "description", parsed(manifest));
    return successful(UI_SETTINGS_VIEW, request, "ui.settingsView");
}

static int is_model_node_id(const char *id) {
    if (id == NULL || id[0] < 'a' || id[0] > 'z') {
        return 0;
    }
    for (const char *at = id + 1; *at != '\0'; at++) {
        int letter_or_digit = (*at >= 'a' && *at <= 'z') || (*at >= 'A' && *at <= 'Z') || (*at >= '0' && *at <= '9');
        if (!letter_or_digit) {
            return 0;
        }
    }
    return 1;
}

static void generates_one_form_holding_one_field_node_per_setting_in_key_order(void) {
    cJSON *view = settings_view_of("{\"id\": \"tools\", \"api\": 1, \"settings\": {"
                                   "\"theme\": {\"type\": \"choice\", \"label\": \"Theme\", \"choices\": [\"light\", \"dark\"]},"
                                   "\"depth\": {\"type\": \"number\", \"label\": \"Depth\"}}}");
    check_text(text_at(view, "title"), "Settings", "the title");
    const cJSON *nodes = cJSON_GetObjectItemCaseSensitive(view, "nodes");
    const cJSON *form = cJSON_GetArrayItem(nodes, 0);
    check_text(text_at(form, "component"), "form", "the first node");
    CHECK(cJSON_GetObjectItemCaseSensitive(form, "parent") == NULL);
    const char *form_id = text_at(form, "id");
    const char *expected[2][2] = {{"depth", "Depth"}, {"theme", "Theme"}};
    CHECK(cJSON_GetArraySize(nodes) == 3);
    for (int index = 0; index < 2; index++) {
        const cJSON *field = cJSON_GetArrayItem(nodes, index + 1);
        check_text(text_at(field, "component"), "field", "a field node");
        check_text(text_at(field, "parent"), form_id, "a field's parent");
        check_text(text_at(field, "field"), expected[index][0], "a field's key");
        check_text(text_at(field, "label"), expected[index][1], "a field's label");
    }
    cJSON_Delete(view);
}

static void gives_every_node_an_id_the_model_accepts_and_no_two_the_same(void) {
    cJSON *view = settings_view_of("{\"id\": \"tools\", \"api\": 1, \"settings\": {"
                                   "\"proxy.port\": {\"type\": \"number\", \"label\": \"Port\"},"
                                   "\"settings\": {\"type\": \"text\", \"label\": \"Named like the form\"}}}");
    const cJSON *nodes = cJSON_GetObjectItemCaseSensitive(view, "nodes");
    CHECK(cJSON_GetArraySize(nodes) == 3);
    const cJSON *node;
    cJSON_ArrayForEach(node, nodes) {
        CHECK(is_model_node_id(text_at(node, "id")));
        for (const cJSON *later = node->next; later != NULL; later = later->next) {
            CHECK(strcmp(text_at(node, "id"), text_at(later, "id")) != 0);
        }
    }
    cJSON_Delete(view);
}

static void generates_an_empty_form_for_a_plugin_declaring_no_settings(void) {
    cJSON *view = settings_view_of("{\"id\": \"tools\", \"api\": 1}");
    check_json_at(view, "nodes", "[{\"id\": \"settingsForm\", \"component\": \"form\"}]", "an empty form");
    cJSON_Delete(view);
}

static void types_each_field_from_its_declaration_with_its_choices_and_its_default(void) {
    cJSON *view = settings_view_of("{\"id\": \"tools\", \"config\": {\"defaults\": {\"mode\": \"a\"}}, \"settings\": {"
                                   "\"token\": {\"type\": \"secret\"},"
                                   "\"mode\": {\"type\": \"choice\", \"choices\": [\"a\", \"b\"]}}}");
    check_json_at(view, "fields",
                  "[{\"choices\": []}, {\"type\": \"choice\", \"choices\": [\"a\", \"b\"], \"default\": \"a\"},"
                  " {\"type\": \"secret\", \"choices\": []}]",
                  "the form untyped, then mode and token in key order");
    cJSON_Delete(view);
}

static void reads_every_field_kind_by_name_and_a_declaration_without_a_type_as_text(void) {
    cJSON *view = settings_view_of("{\"id\": \"tools\", \"settings\": {"
                                   "\"a\": {\"type\": \"text\"}, \"b\": {\"type\": \"secret\"}, \"c\": {\"type\": \"number\"},"
                                   "\"d\": {\"type\": \"toggle\"}, \"e\": {\"type\": \"choice\"},"
                                   "\"f\": {\"type\": \"multi-choice\", \"choices\": [\"x\"]}, \"g\": {\"type\": \"path\"},"
                                   "\"h\": {\"type\": \"list\"}, \"i\": {\"type\": \"key-value\"}, \"j\": {\"label\": \"J\"}}}");
    const char *expected[] = {"text", "secret", "number", "toggle", "choice", "multi-choice", "path", "list", "key-value",
                              "text"};
    const cJSON *fields = cJSON_GetObjectItemCaseSensitive(view, "fields");
    CHECK(cJSON_GetArraySize(fields) == 11);
    for (int index = 0; index < 10; index++) {
        check_text(text_at(cJSON_GetArrayItem(fields, index + 1), "type"), expected[index], "a field's kind");
    }
    cJSON_Delete(view);
}

static void reads_a_default_at_a_dotted_key_through_the_defaults_tree(void) {
    cJSON *view = settings_view_of("{\"id\": \"tools\", \"config\": {\"defaults\": {\"proxy\": {\"port\": 8080}}},"
                                   " \"settings\": {\"proxy.port\": {\"type\": \"number\"}, \"proxy.host\": {}}}");
    check_json_at(view, "fields",
                  "[{\"choices\": []}, {\"type\": \"text\", \"choices\": []},"
                  " {\"type\": \"number\", \"choices\": [], \"default\": 8080}]",
                  "a nested default and a missing one");
    cJSON_Delete(view);
}

static void refuses_a_field_type_it_does_not_know_and_choices_that_are_not_texts(void) {
    const char *requests[] = {
        "{\"description\": {\"id\": \"tools\", \"settings\": {\"a\": {\"type\": \"colour\"}}}}",
        "{\"description\": {\"id\": \"tools\", \"settings\": {\"a\": {\"type\": 3}}}}",
        "{\"description\": {\"id\": \"tools\", \"settings\": {\"a\": {\"type\": \"choice\", \"choices\": \"a\"}}}}",
        "{\"description\": {\"id\": \"tools\", \"settings\": {\"a\": {\"type\": \"choice\", \"choices\": [1]}}}}",
        "{\"description\": {\"id\": \"tools\", \"settings\": {\"a\": \"text\"}}}"};
    for (size_t index = 0; index < sizeof requests / sizeof requests[0]; index++) {
        expect_ui_refusal(ui_call(UI_SETTINGS_VIEW, parsed(requests[index])), SPISOR_MALFORMED, "setting",
                          requests[index]);
    }
}

static void refuses_a_description_that_is_no_manifest_with_an_id(void) {
    const char *requests[] = {"{}", "{\"description\": []}", "{\"description\": {\"api\": 1}}"};
    for (size_t index = 0; index < sizeof requests / sizeof requests[0]; index++) {
        expect_ui_refusal(ui_call(UI_SETTINGS_VIEW, parsed(requests[index])), SPISOR_MALFORMED, "description",
                          requests[index]);
    }
}

int main(void) {
    generates_one_form_holding_one_field_node_per_setting_in_key_order();
    gives_every_node_an_id_the_model_accepts_and_no_two_the_same();
    generates_an_empty_form_for_a_plugin_declaring_no_settings();
    types_each_field_from_its_declaration_with_its_choices_and_its_default();
    reads_every_field_kind_by_name_and_a_declaration_without_a_type_as_text();
    reads_a_default_at_a_dotted_key_through_the_defaults_tree();
    refuses_a_field_type_it_does_not_know_and_choices_that_are_not_texts();
    refuses_a_description_that_is_no_manifest_with_an_id();
    return check_summary("ui_settings_view_test");
}
