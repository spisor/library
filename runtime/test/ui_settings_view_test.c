#include <stdio.h>
#include <string.h>

#include "ui_fixture.h"

static cJSON *settings_view_of(const char *manifest) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddItemToObject(request, "description", parsed(manifest));
    return successful(runtime_ui_settings_view, request, "ui.settingsView");
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

static void refuses_a_description_that_is_no_manifest_with_an_id(void) {
    const char *requests[] = {"{}", "{\"description\": []}", "{\"description\": {\"api\": 1}}"};
    for (size_t index = 0; index < sizeof requests / sizeof requests[0]; index++) {
        catalog_answer answer = call_catalog(runtime_ui_settings_view, parsed(requests[index]));
        expect_refusal(answer, "description", requests[index]);
        cJSON_Delete(answer.tree);
    }
}

int main(void) {
    generates_one_form_holding_one_field_node_per_setting_in_key_order();
    gives_every_node_an_id_the_model_accepts_and_no_two_the_same();
    generates_an_empty_form_for_a_plugin_declaring_no_settings();
    refuses_a_description_that_is_no_manifest_with_an_id();
    return check_summary("ui_settings_view_test");
}
