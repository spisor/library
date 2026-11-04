#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_fixture.h"

#define VIEW_NODES "\"nodes\": [{\"id\": \"root\", \"component\": \"stack\"}]"

static const char *TOOLS =
    "{\"id\": \"tools\", \"api\": 1, \"views\": {"
    "\"beta\": {\"title\": \"Beta\", " VIEW_NODES "}, \"alpha\": {\"title\": \"Alpha\", " VIEW_NODES "},"
    "\"gamma\": {\"title\": \"Gamma\", " VIEW_NODES "}, \"delta\": {\"title\": \"Delta\", " VIEW_NODES "}},"
    "\"settings\": {\"depth\": {\"type\": \"number\", \"label\": \"Depth\"}},"
    "\"actions\": {\"sync\": {\"label\": \"Sync\", \"call\": {\"service\": \"tools:main\", \"method\": \"sync\"}},"
    "\"clear\": {\"label\": \"Clear\", \"call\": {\"service\": \"tools:main\", \"method\": \"clear\"}}}}";

static const char *NOTES = "{\"id\": \"notes\", \"api\": 1, \"views\": {\"list\": {\"title\": \"List\", " VIEW_NODES "}}}";

static const char *ARCHIVE = "{\"id\": \"archive\", \"api\": 1, \"views\": {\"old\": {\"title\": \"Old\", " VIEW_NODES "}}}";

/** ui.navigation of the manifests given as JSON texts, under the ui settings given as JSON text. */
static cJSON *navigation_of(const char *const *manifests, size_t count, const char *ui) {
    cJSON *request = cJSON_CreateObject();
    cJSON *descriptions = cJSON_AddArrayToObject(request, "descriptions");
    for (size_t index = 0; index < count; index++) {
        cJSON_AddItemToArray(descriptions, parsed(manifests[index]));
    }
    cJSON_AddItemToObject(request, "ui", parsed(ui));
    return successful(runtime_ui_navigation, request, "ui.navigation");
}

/** The entries as plugin/kind/id keys joined by commas. */
static void check_keys(const cJSON *navigation, const char *expected, const char *label) {
    char keys[1024] = "";
    const cJSON *entry;
    cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(navigation, "entries")) {
        char key[128];
        snprintf(key, sizeof key, "%s%s/%s/%s", keys[0] == '\0' ? "" : ",", text_at(entry, "plugin"),
                 text_at(entry, "kind"), text_at(entry, "id"));
        strncat(keys, key, sizeof keys - strlen(keys) - 1);
    }
    check_text(keys, expected, label);
}

/** The distinct plugins of the entries, in order, joined by commas. */
static void check_plugins(const cJSON *navigation, const char *expected, const char *label) {
    char plugins[256] = "";
    const char *previous = NULL;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(navigation, "entries")) {
        const char *plugin = text_at(entry, "plugin");
        if (previous == NULL || strcmp(previous, plugin) != 0) {
            strncat(plugins, previous == NULL ? "" : ",", sizeof plugins - strlen(plugins) - 1);
            strncat(plugins, plugin, sizeof plugins - strlen(plugins) - 1);
        }
        previous = plugin;
    }
    check_text(plugins, expected, label);
}

static const cJSON *entry_with(const cJSON *navigation, const char *member, const char *value) {
    const cJSON *entry;
    cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(navigation, "entries")) {
        if (strcmp(text_at(entry, member), value) == 0) {
            return entry;
        }
    }
    return NULL;
}

static void orders_plugins_by_their_order_then_by_id_with_unordered_plugins_last(void) {
    const char *manifests[] = {TOOLS, NOTES, ARCHIVE};
    cJSON *navigation = navigation_of(manifests, 3, "{\"plugins\": {\"notes\": {\"order\": 1}, \"tools\": {\"order\": 2}}}");
    check_plugins(navigation, "notes,tools,archive", "ordered plugins");
    cJSON_Delete(navigation);
}

static void orders_plugins_sharing_an_order_by_id(void) {
    const char *manifests[] = {TOOLS, NOTES, ARCHIVE};
    cJSON *navigation = navigation_of(manifests, 3, "{}");
    check_plugins(navigation, "archive,notes,tools", "plugins by id");
    cJSON_Delete(navigation);
}

static void puts_the_default_view_first_then_pinned_then_views_by_key_then_settings_then_actions(void) {
    const char *manifests[] = {TOOLS};
    cJSON *navigation =
        navigation_of(manifests, 1, "{\"plugins\": {\"tools\": {\"defaultView\": \"gamma\", \"pinned\": [\"delta\", \"beta\"]}}}");
    check_keys(navigation,
               "tools/view/gamma,tools/view/delta,tools/view/beta,tools/view/alpha,tools/settings/settings,"
               "tools/action/clear,tools/action/sync",
               "entry order");
    cJSON_Delete(navigation);
}

static void marks_pinned_views_pinned_and_titles_every_entry(void) {
    const char *manifests[] = {TOOLS};
    cJSON *navigation = navigation_of(manifests, 1, "{\"plugins\": {\"tools\": {\"pinned\": [\"delta\"]}}}");
    const cJSON *delta = entry_with(navigation, "id", "delta");
    check_json(delta == NULL ? NULL : cJSON_Duplicate(delta, 1),
               "{\"plugin\": \"tools\", \"kind\": \"view\", \"id\": \"delta\", \"title\": \"Delta\", \"pinned\": true}",
               "the pinned entry");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(entry_with(navigation, "id", "alpha"), "pinned")));
    check_text(text_at(entry_with(navigation, "kind", "settings"), "title"), "Settings", "the settings title");
    check_text(text_at(entry_with(navigation, "id", "sync"), "title"), "Sync", "the action title");
    cJSON_Delete(navigation);
}

static void leaves_a_hidden_plugin_out_entirely(void) {
    const char *manifests[] = {TOOLS, NOTES};
    cJSON *navigation = navigation_of(manifests, 2, "{\"plugins\": {\"tools\": {\"hidden\": true}}}");
    check_keys(navigation, "notes/view/list", "a hidden plugin");
    cJSON_Delete(navigation);
}

static void ignores_an_unknown_plugin_id_and_a_pinned_or_default_view_that_does_not_exist(void) {
    const char *manifests[] = {NOTES};
    cJSON *navigation = navigation_of(manifests, 1,
                                      "{\"plugins\": {\"gone\": {\"order\": 0, \"hidden\": true}, \"notes\": {\"pinned\": "
                                      "[\"missing\"], \"defaultView\": \"absent\"}}}");
    check_keys(navigation, "notes/view/list", "unknown customisation");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(entry_with(navigation, "id", "list"), "pinned")));
    cJSON_Delete(navigation);
}

static void adds_no_settings_entry_for_a_plugin_declaring_no_settings(void) {
    const char *manifests[] = {NOTES};
    cJSON *navigation = navigation_of(manifests, 1, "{}");
    CHECK(navigation != NULL && entry_with(navigation, "kind", "settings") == NULL);
    cJSON_Delete(navigation);
}

static void passes_everything_under_ui_but_plugins_through_as_preferences(void) {
    const char *manifests[] = {NOTES};
    cJSON *navigation =
        navigation_of(manifests, 1, "{\"theme\": \"dark\", \"density\": \"compact\", \"plugins\": {\"notes\": {\"order\": 1}}}");
    check_json_at(navigation, "preferences", "{\"theme\": \"dark\", \"density\": \"compact\"}", "preferences");
    cJSON_Delete(navigation);
}

static void ignores_customisation_values_of_the_wrong_type(void) {
    const char *manifests[] = {TOOLS, NOTES};
    cJSON *navigation = navigation_of(manifests, 2,
                                      "{\"plugins\": {\"tools\": {\"hidden\": \"yes\", \"pinned\": \"delta\", \"defaultView\": 3,"
                                      " \"order\": \"first\"}, \"notes\": {\"order\": 1.5}}}");
    const cJSON *entries = cJSON_GetObjectItemCaseSensitive(navigation, "entries");
    check_text(text_at(cJSON_GetArrayItem(entries, 0), "plugin"), "notes", "the first plugin");
    check_text(text_at(cJSON_GetArrayItem(entries, 1), "id"), "alpha", "the second entry");
    const cJSON *entry;
    cJSON_ArrayForEach(entry, entries) {
        CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(entry, "pinned")));
    }
    cJSON_Delete(navigation);
}

static void sorts_a_fractional_order_as_no_order_after_every_integer_one(void) {
    const char *manifests[] = {TOOLS, NOTES};
    cJSON *navigation = navigation_of(manifests, 2, "{\"plugins\": {\"tools\": {\"order\": 2}, \"notes\": {\"order\": 1.5}}}");
    check_plugins(navigation, "tools,notes", "a fractional order");
    cJSON_Delete(navigation);
}

static void adds_no_settings_entry_for_a_plugin_declaring_an_empty_settings_object(void) {
    const char *manifests[] = {"{\"id\": \"bare\", \"api\": 1, \"settings\": {}}"};
    cJSON *navigation = navigation_of(manifests, 1, "{}");
    check_json_at(navigation, "entries", "[]", "empty settings");
    cJSON_Delete(navigation);
}

static void keeps_two_plugins_of_one_order_in_id_order_whatever_order_they_arrive_in(void) {
    const char *manifests[] = {ARCHIVE, TOOLS, NOTES};
    cJSON *navigation = navigation_of(
        manifests, 3, "{\"plugins\": {\"tools\": {\"order\": 1}, \"archive\": {\"order\": 1}, \"notes\": {\"order\": -2}}}");
    check_plugins(navigation, "notes,archive,tools", "equal orders");
    cJSON_Delete(navigation);
}

static void refuses_descriptions_that_are_no_list_of_manifests_with_an_id(void) {
    const char *requests[] = {"{\"ui\": {}}", "{\"descriptions\": {}, \"ui\": {}}",
                              "{\"descriptions\": [{\"api\": 1}], \"ui\": {}}", "{\"descriptions\": [], \"ui\": []}"};
    for (size_t index = 0; index < sizeof requests / sizeof requests[0]; index++) {
        catalog_answer answer = call_catalog(runtime_ui_navigation, parsed(requests[index]));
        expect_refusal(answer, "descriptions", requests[index]);
        cJSON_Delete(answer.tree);
    }
}

int main(void) {
    orders_plugins_by_their_order_then_by_id_with_unordered_plugins_last();
    orders_plugins_sharing_an_order_by_id();
    puts_the_default_view_first_then_pinned_then_views_by_key_then_settings_then_actions();
    marks_pinned_views_pinned_and_titles_every_entry();
    leaves_a_hidden_plugin_out_entirely();
    ignores_an_unknown_plugin_id_and_a_pinned_or_default_view_that_does_not_exist();
    adds_no_settings_entry_for_a_plugin_declaring_no_settings();
    passes_everything_under_ui_but_plugins_through_as_preferences();
    ignores_customisation_values_of_the_wrong_type();
    sorts_a_fractional_order_as_no_order_after_every_integer_one();
    adds_no_settings_entry_for_a_plugin_declaring_an_empty_settings_object();
    keeps_two_plugins_of_one_order_in_id_order_whatever_order_they_arrive_in();
    refuses_descriptions_that_are_no_list_of_manifests_with_an_id();
    return check_summary("ui_navigation_test");
}
