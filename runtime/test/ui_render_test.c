#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_fixture.h"

/**
 * What the layer's engine answered for one bound method: outcome is {"answer": ...}, {"error": ...} or
 * {"unavailable": true}, and failing, when set, is the one check the answer did not pass, with issue.
 */
typedef struct binding_fixture {
    const char *method;
    const char *outcome;
    const char *failing;
    const char *issue;
} binding_fixture;

#define SERIES "[{\"label\": \"load\", \"points\": [1, 2, 3]}]"

static const binding_fixture VIEWER_STATUS[] = {
    {"rows", "{\"answer\": {\"items\": [{\"id\": \"x\", \"state\": \"detected\"}]}}", NULL, NULL},
    {"wrongShape", "{\"answer\": {\"items\": \"oops\"}}", "table",
     "{\"message\": \"items must be an array\", \"fix\": \"answer items as an array of rows\", \"path\": \"items\"}"},
    {"bare", "{\"answer\": \"oops\"}", "table", "{\"message\": \"the answer must be an object\", \"fix\": \"answer an object\"}"},
    {"greeting", "{\"answer\": {\"text\": \"hello\"}}", NULL, NULL},
    {"fails",
     "{\"error\": {\"detail\": \"viewer:status.fails threw: the status file is unreadable\", \"fix\": \"fix viewer\"}}", NULL,
     NULL},
    {"hangs", "{\"error\": {\"detail\": \"viewer:status.hangs did not answer within 5000 ms\", \"fix\": \"answer sooner\"}}",
     NULL, NULL},
    {"series", "{\"answer\": {\"series\": " SERIES "}}", "progress",
     "{\"message\": \"must have required property value\", \"fix\": \"answer a value and a maximum\"}"},
    {"seriesAndMeasure", "{\"answer\": {\"series\": " SERIES ", \"value\": 3, \"maximum\": 10}}", NULL, NULL},
    {"seriesAndBadMeasure", "{\"answer\": {\"series\": " SERIES ", \"value\": \"three\", \"maximum\": 10}}", "progress",
     "{\"message\": \"value must be a number\", \"fix\": \"answer a number\", \"path\": \"value\"}"},
    {"measure", "{\"answer\": {\"value\": 3, \"maximum\": 10}}", "chart",
     "{\"message\": \"must have required property series\", \"fix\": \"answer series\", \"path\": \"series\"}"},
    {"measureAndBadSeries", "{\"answer\": {\"value\": 3, \"maximum\": 10, \"series\": \"load\"}}", "chart",
     "{\"message\": \"series must be an array\", \"fix\": \"answer series as an array\", \"path\": \"series\"}"},
};

#define VIEWER_STATUS_COUNT (sizeof VIEWER_STATUS / sizeof VIEWER_STATUS[0])

static const binding_fixture *fixture_for(const binding_fixture *fixtures, size_t count, const char *method) {
    for (size_t index = 0; method != NULL && index < count; index++) {
        if (strcmp(fixtures[index].method, method) == 0) {
            return &fixtures[index];
        }
    }
    fprintf(stderr, "no binding fixture answers %s\n", method == NULL ? "a call naming no method" : method);
    exit(2);
}

/** The checked list of an answer: one entry per check the call asked for, failing only the fixture's own. */
static cJSON *checked_for(const cJSON *checks, const binding_fixture *fixture) {
    cJSON *checked = cJSON_CreateArray();
    const cJSON *check;
    cJSON_ArrayForEach(check, checks) {
        cJSON *entry = cJSON_Duplicate(check, 1);
        cJSON *issues = cJSON_AddArrayToObject(entry, "issues");
        if (fixture->failing != NULL && strcmp(text_at(check, "against"), fixture->failing) == 0) {
            cJSON_AddItemToArray(issues, parsed(fixture->issue));
        }
        cJSON_AddItemToArray(checked, entry);
    }
    return checked;
}

/** What the layer hands back for one call of ui.plan: the call itself with the engine's outcome. */
static cJSON *answer_to(const cJSON *call, const binding_fixture *fixtures, size_t count) {
    const char *method = text_at(cJSON_GetObjectItemCaseSensitive(call, "binding"), "method");
    const binding_fixture *fixture = fixture_for(fixtures, count, method);
    cJSON *answer = cJSON_CreateObject();
    cJSON_AddItemToObject(answer, "plugin", cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(call, "plugin"), 1));
    cJSON_AddItemToObject(answer, "binding", cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(call, "binding"), 1));
    cJSON_AddItemToObject(answer, "arguments", cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(call, "arguments"), 1));
    cJSON *outcome = parsed(fixture->outcome);
    while (outcome->child != NULL) {
        cJSON *member = cJSON_DetachItemViaPointer(outcome, outcome->child);
        cJSON_AddItemToObject(answer, member->string, member);
    }
    cJSON_Delete(outcome);
    if (cJSON_GetObjectItemCaseSensitive(answer, "answer") != NULL) {
        cJSON_AddItemToObject(answer, "checked", checked_for(cJSON_GetObjectItemCaseSensitive(call, "checks"), fixture));
    }
    return answer;
}

static cJSON *screen_request(const cJSON *descriptions, const char *screen, const cJSON *answers) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddItemToObject(request, "descriptions", cJSON_Duplicate(descriptions, 1));
    cJSON_AddItemToObject(request, "screen", parsed(screen));
    if (answers != NULL) {
        cJSON_AddItemToObject(request, "answers", cJSON_Duplicate(answers, 1));
    }
    return request;
}

static cJSON *plan_of(const cJSON *descriptions, const char *screen, const cJSON *answers) {
    return successful(runtime_ui_plan, screen_request(descriptions, screen, answers), "ui.plan");
}

/** Every call one plan asked for, answered from fixtures. */
static cJSON *answers_to(const cJSON *plan, const binding_fixture *fixtures, size_t count) {
    cJSON *answers = cJSON_CreateArray();
    const cJSON *call;
    cJSON_ArrayForEach(call, cJSON_GetObjectItemCaseSensitive(plan, "calls")) {
        cJSON_AddItemToArray(answers, answer_to(call, fixtures, count));
    }
    return answers;
}

/** Plans the screen, answers every call from fixtures, confirms the plan is then done, and renders it. */
static cJSON *rendered_with(cJSON *descriptions, const char *screen, const binding_fixture *fixtures, size_t count) {
    cJSON *plan = plan_of(descriptions, screen, NULL);
    cJSON *answers = answers_to(plan, fixtures, count);
    cJSON *second = plan_of(descriptions, screen, answers);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(second, "done")));
    cJSON *screen_tree = successful(runtime_ui_render, screen_request(descriptions, screen, answers), "ui.render");
    cJSON_Delete(plan);
    cJSON_Delete(second);
    cJSON_Delete(answers);
    cJSON_Delete(descriptions);
    return screen_tree;
}

static cJSON *rendered(const char *plugin, const char *screen, const binding_fixture *fixtures, size_t count) {
    const char *plugins[] = {plugin};
    return rendered_with(fixture_descriptions(plugins, 1), screen, fixtures, count);
}

static cJSON *viewer_view(const char *view) {
    char screen[256];
    snprintf(screen, sizeof screen, "{\"plugin\": \"viewer\", \"kind\": \"view\", \"id\": \"%s\"}", view);
    return rendered("viewer", screen, VIEWER_STATUS, VIEWER_STATUS_COUNT);
}

static const cJSON *node_of(const cJSON *screen, const char *id) {
    const cJSON *node;
    cJSON_ArrayForEach(node, cJSON_GetObjectItemCaseSensitive(screen, "nodes")) {
        if (strcmp(text_at(node, "id"), id) == 0) {
            return node;
        }
    }
    fprintf(stderr, "no node %s\n", id);
    check_failures++;
    return NULL;
}

static void check_state(const cJSON *screen, const char *id, const char *state) {
    check_text(text_at(node_of(screen, id), "state"), state, id);
}

static void check_absent(const cJSON *screen, const char *id, const char *member) {
    if (cJSON_GetObjectItemCaseSensitive(node_of(screen, id), member) != NULL) {
        fprintf(stderr, "%s carries %s\n", id, member);
        check_failures++;
    }
}

static void renders_a_bound_table_ready_with_what_its_binding_answered(void) {
    cJSON *screen = viewer_view("overview");
    check_text(text_at(screen, "plugin"), "viewer", "the plugin");
    check_text(text_at(screen, "kind"), "view", "the kind");
    check_text(text_at(screen, "view"), "overview", "the view");
    check_text(text_at(screen, "title"), "Overview", "the title");
    const cJSON *rows = node_of(screen, "rows");
    check_text(text_at(rows, "component"), "table", "the rows component");
    check_text(text_at(rows, "parent"), "main", "the rows parent");
    check_state(screen, "rows", "ready");
    const cJSON *row = cJSON_GetArrayItem(value_at(rows, "data/items"), 0);
    check_text(text_at(row, "id"), "x", "the row id");
    check_text(text_at(row, "state"), "detected", "the row state");
    check_text(text_at(node_of(screen, "main"), "component"), "stack", "the main component");
    check_state(screen, "main", "ready");
    check_absent(screen, "main", "parent");
    check_absent(screen, "main", "data");
    cJSON_Delete(screen);
}

static void puts_only_the_node_whose_answer_has_the_wrong_shape_in_error_with_the_path_at_fault(void) {
    cJSON *screen = viewer_view("wrongShape");
    check_state(screen, "rows", "error");
    check_json_at(node_of(screen, "rows"), "error",
                  "{\"detail\": \"items must be an array\", \"fix\": \"answer items as an array of rows\", \"path\": \"items\"}",
                  "the error");
    check_absent(screen, "rows", "data");
    check_state(screen, "greeting", "ready");
    check_json_at(node_of(screen, "greeting"), "data", "{\"text\": \"hello\"}", "the greeting");
    cJSON_Delete(screen);
}

static void puts_a_node_answering_a_bare_string_in_error(void) {
    cJSON *screen = viewer_view("bare");
    check_state(screen, "rows", "error");
    check_json_at(node_of(screen, "rows"), "error", "{\"detail\": \"the answer must be an object\", \"fix\": \"answer an object\"}",
                  "an error with no path");
    cJSON_Delete(screen);
}

static void puts_a_node_whose_binding_failed_in_error_and_renders_its_sibling(void) {
    cJSON *screen = viewer_view("failing");
    check_state(screen, "broken", "error");
    const char *detail = text_at(value_at(node_of(screen, "broken"), "error"), "detail");
    CHECK(detail != NULL && strstr(detail, "the status file is unreadable") != NULL);
    check_absent(screen, "broken", "data");
    check_state(screen, "greeting", "ready");
    cJSON_Delete(screen);
}

static void puts_a_node_whose_binding_never_answered_in_error(void) {
    cJSON *screen = viewer_view("hanging");
    check_state(screen, "slow", "error");
    cJSON_Delete(screen);
}

static void reads_the_branch_the_nodes_chart_names_never_the_one_the_answer_carries(void) {
    cJSON *screen = viewer_view("charts");
    check_state(screen, "trend", "ready");
    check_json_at(node_of(screen, "trend"), "data", "{\"series\": " SERIES "}", "the trend");
    check_state(screen, "usage", "ready");
    check_json_at(node_of(screen, "usage"), "data", "{\"value\": 3, \"maximum\": 10}", "the usage");
    check_state(screen, "gauge", "error");
    cJSON_Delete(screen);
}

static void checks_a_chart_answer_against_the_branch_its_node_selects(void) {
    cJSON *screen = viewer_view("charts");
    check_state(screen, "badMeter", "error");
    check_text(text_at(value_at(node_of(screen, "badMeter"), "error"), "path"), "value", "the bad meter path");
    check_absent(screen, "badMeter", "data");
    check_state(screen, "badTrend", "error");
    check_text(text_at(value_at(node_of(screen, "badTrend"), "error"), "path"), "series", "the bad trend path");
    check_absent(screen, "badTrend", "data");
    check_state(screen, "measureTrend", "error");
    check_text(text_at(value_at(node_of(screen, "measureTrend"), "error"), "path"), "series", "the measure trend path");
    cJSON_Delete(screen);
}

static void carries_only_the_known_node_members_in_props(void) {
    cJSON *screen = viewer_view("overview");
    check_json_at(node_of(screen, "rows"), "props", "{\"tone\": \"accent\", \"span\": 6, \"action\": \"restart\"}", "the props");
    check_json_at(node_of(screen, "main"), "props", "{}", "no props");
    cJSON_Delete(screen);
}

static void renders_a_bound_node_unavailable_when_its_plugin_is_not_running(void) {
    static const binding_fixture DORMANT[] = {{"read", "{\"unavailable\": true}", NULL, NULL}};
    cJSON *screen = rendered("dormant", "{\"plugin\": \"dormant\", \"kind\": \"view\", \"id\": \"status\"}", DORMANT, 1);
    check_state(screen, "summary", "unavailable");
    check_absent(screen, "summary", "data");
    check_absent(screen, "summary", "error");
    check_state(screen, "heading", "ready");
    check_json_at(node_of(screen, "heading"), "props", "{\"text\": \"Dormant\"}", "the heading props");
    cJSON_Delete(screen);
}

static void renders_the_generated_settings_screen_through_the_same_path_as_a_view(void) {
    cJSON *screen = rendered("viewer", "{\"plugin\": \"viewer\", \"kind\": \"settings\", \"id\": \"settings\"}",
                             VIEWER_STATUS, VIEWER_STATUS_COUNT);
    check_text(text_at(screen, "plugin"), "viewer", "the plugin");
    check_text(text_at(screen, "kind"), "settings", "the kind");
    check_text(text_at(screen, "title"), "Settings", "the title");
    CHECK(cJSON_GetObjectItemCaseSensitive(screen, "view") == NULL);
    char fields[128] = "";
    const cJSON *node;
    cJSON_ArrayForEach(node, cJSON_GetObjectItemCaseSensitive(screen, "nodes")) {
        check_text(text_at(node, "state"), "ready", "a settings node");
        if (strcmp(text_at(node, "component"), "field") == 0) {
            strncat(fields, fields[0] == '\0' ? "" : ",", sizeof fields - strlen(fields) - 1);
            strncat(fields, text_at(value_at(node, "props"), "field"), sizeof fields - strlen(fields) - 1);
        }
    }
    check_text(fields, "depth,theme", "the fields");
    cJSON_Delete(screen);
}

static void renders_a_declared_view_named_settings_as_itself_and_keeps_the_generated_screen_reachable(void) {
    cJSON *declared = viewer_view("settings");
    check_text(text_at(declared, "kind"), "view", "the declared kind");
    check_text(text_at(declared, "view"), "settings", "the declared view");
    check_text(text_at(declared, "title"), "Declared settings", "the declared title");
    cJSON_Delete(declared);
    const char *plugins[] = {"viewer"};
    cJSON *request = cJSON_CreateObject();
    cJSON_AddItemToObject(request, "descriptions", fixture_descriptions(plugins, 1));
    cJSON_AddItemToObject(request, "ui", cJSON_CreateObject());
    cJSON *navigation = successful(runtime_ui_navigation, request, "ui.navigation");
    const cJSON *entry;
    int screens = 0;
    cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(navigation, "entries")) {
        int is_view_called_settings = strcmp(text_at(entry, "kind"), "view") == 0 && strcmp(text_at(entry, "id"), "settings") == 0;
        int is_settings = strcmp(text_at(entry, "kind"), "settings") == 0;
        if (!is_view_called_settings && !is_settings) {
            continue;
        }
        char *text = cJSON_PrintUnformatted(entry);
        cJSON *screen = rendered("viewer", text, VIEWER_STATUS, VIEWER_STATUS_COUNT);
        cJSON_free(text);
        check_text(text_at(screen, "title"), is_settings ? "Settings" : "Declared settings", "an entry's screen");
        if (is_settings) {
            check_text(text_at(screen, "kind"), "settings", "the generated kind");
            check_text(text_at(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(screen, "nodes"), 0), "component"), "form",
                       "the generated first node");
        }
        cJSON_Delete(screen);
        screens++;
    }
    CHECK(screens == 2);
    cJSON_Delete(navigation);
}

static void expect_screen_refused(const char *const *plugins, size_t count, const char *screen, const char *fragment) {
    cJSON *descriptions = fixture_descriptions(plugins, count);
    cJSON *answers = cJSON_CreateArray();
    catalog_answer plan = call_catalog(runtime_ui_plan, screen_request(descriptions, screen, NULL));
    expect_refusal(plan, fragment, "ui.plan");
    catalog_answer render = call_catalog(runtime_ui_render, screen_request(descriptions, screen, answers));
    expect_refusal(render, fragment, "ui.render");
    cJSON_Delete(plan.tree);
    cJSON_Delete(render.tree);
    cJSON_Delete(answers);
    cJSON_Delete(descriptions);
}

static void refuses_to_render_an_action_entry_as_a_screen(void) {
    const char *plugins[] = {"viewer"};
    expect_screen_refused(plugins, 1,
                          "{\"plugin\": \"viewer\", \"kind\": \"action\", \"id\": \"restart\", \"title\": \"Restart\", "
                          "\"pinned\": false}",
                          "restart");
}

static void puts_a_node_in_error_not_ready_with_another_plugins_answer_when_two_provide_its_service(void) {
    static const binding_fixture SILENT[] = {
        {"greet",
         "{\"error\": {\"detail\": \"demo:greeting is also provided by greeter, so silent's own implementation cannot be told "
         "apart\", \"fix\": \"disable all but one plugin providing demo:greeting\"}}",
         NULL, NULL}};
    const char *plugins[] = {"greeter", "silent"};
    cJSON *screen = rendered_with(fixture_descriptions(plugins, 2), "{\"plugin\": \"silent\", \"kind\": \"view\", \"id\": \"hello\"}",
                                  SILENT, 1);
    check_state(screen, "message", "error");
    check_absent(screen, "message", "data");
    cJSON_Delete(screen);
}

static void renders_a_quarantined_plugins_node_unavailable_while_another_provides_the_same_service(void) {
    static const binding_fixture RIVAL[] = {{"greet", "{\"unavailable\": true}", NULL, NULL}};
    const char *plugins[] = {"greeter", "rival"};
    cJSON *screen =
        rendered_with(fixture_descriptions(plugins, 2), "{\"plugin\": \"rival\", \"kind\": \"view\", \"id\": \"hello\"}", RIVAL, 1);
    check_state(screen, "message", "unavailable");
    check_absent(screen, "message", "data");
    cJSON_Delete(screen);
}

static void refuses_a_view_the_plugin_does_not_declare(void) {
    const char *plugins[] = {"viewer"};
    expect_screen_refused(plugins, 1, "{\"plugin\": \"viewer\", \"kind\": \"view\", \"id\": \"nowhere\"}", "nowhere");
}

static void refuses_a_declared_view_that_carries_no_nodes_or_a_node_with_no_id(void) {
    cJSON *descriptions = cJSON_CreateArray();
    cJSON_AddItemToArray(descriptions, parsed("{\"id\": \"hollow\", \"api\": 1, \"views\": {\"empty\": {\"title\": \"Empty\"}}}"));
    const char *screen = "{\"plugin\": \"hollow\", \"kind\": \"view\", \"id\": \"empty\"}";
    catalog_answer plan = call_catalog(runtime_ui_plan, screen_request(descriptions, screen, NULL));
    expect_refusal(plan, "empty", "a view of no nodes");
    cJSON_Delete(plan.tree);
    cJSON_Delete(descriptions);
    descriptions = cJSON_CreateArray();
    cJSON_AddItemToArray(descriptions, parsed("{\"id\": \"hollow\", \"api\": 1, \"views\": {\"empty\": {\"title\": \"Empty\", "
                                              "\"nodes\": [{\"component\": \"text\", \"bind\": {\"service\": \"s\", \"method\": \"m\"}}]}}}"));
    cJSON *answers = cJSON_CreateArray();
    catalog_answer render = call_catalog(runtime_ui_render, screen_request(descriptions, screen, answers));
    expect_refusal(render, "empty", "a node with no id");
    cJSON_Delete(render.tree);
    cJSON_Delete(answers);
    cJSON_Delete(descriptions);
}

static void refuses_a_screen_of_a_plugin_no_description_names(void) {
    const char *plugins[] = {"viewer"};
    expect_screen_refused(plugins, 1, "{\"plugin\": \"ghost\", \"kind\": \"view\", \"id\": \"overview\"}", "ghost");
    expect_screen_refused(plugins, 1, "{\"plugin\": \"viewer\", \"kind\": \"view\"}", "screen");
    expect_screen_refused(plugins, 1, "{\"kind\": \"settings\"}", "screen");
}

static void gives_a_plugins_own_detected_row_the_unavailable_treatment_in_every_items_family(void) {
    cJSON *screen = viewer_view("families");
    const char *families[] = {"asList", "asTable", "asTree"};
    for (size_t index = 0; index < 3; index++) {
        check_json_at(node_of(screen, families[index]), "data",
                      "{\"items\": [{\"id\": \"x\", \"state\": \"detected\", \"unavailable\": true, \"install\": true}]}",
                      families[index]);
    }
    cJSON_Delete(screen);
}

static const cJSON *call_for(const cJSON *plan, const char *method) {
    const cJSON *call;
    cJSON_ArrayForEach(call, cJSON_GetObjectItemCaseSensitive(plan, "calls")) {
        if (strcmp(text_at(cJSON_GetObjectItemCaseSensitive(call, "binding"), "method"), method) == 0) {
            return call;
        }
    }
    fprintf(stderr, "no call of %s\n", method);
    check_failures++;
    return NULL;
}

static void plans_each_binding_once_with_every_check_its_nodes_need(void) {
    const char *plugins[] = {"viewer"};
    cJSON *descriptions = fixture_descriptions(plugins, 1);
    cJSON *families = plan_of(descriptions, "{\"plugin\": \"viewer\", \"kind\": \"view\", \"id\": \"families\"}", NULL);
    check_json_at(families, "calls",
                  "[{\"plugin\": \"viewer\", \"binding\": {\"service\": \"viewer:status\", \"method\": \"rows\"}, \"arguments\": {},"
                  " \"checks\": [{\"against\": \"list\"}, {\"against\": \"table\"}, {\"against\": \"tree\"}]}]",
                  "one call for three nodes");
    cJSON *charts = plan_of(descriptions, "{\"plugin\": \"viewer\", \"kind\": \"view\", \"id\": \"charts\"}", NULL);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(charts, "calls")) == 5);
    check_json_at(call_for(charts, "seriesAndMeasure"), "checks",
                  "[{\"against\": \"chart\", \"members\": [\"series\"]}, {\"against\": \"progress\"}]", "both branches");
    check_json_at(call_for(charts, "series"), "checks", "[{\"against\": \"progress\"}]", "the meter's branch");
    check_json_at(call_for(charts, "measure"), "checks", "[{\"against\": \"chart\", \"members\": [\"series\"]}]",
                  "the series branch");
    cJSON_Delete(families);
    cJSON_Delete(charts);
    cJSON_Delete(descriptions);
}

static void plans_only_the_bindings_still_unanswered_and_is_done_for_a_screen_binding_nothing(void) {
    const char *plugins[] = {"viewer"};
    cJSON *descriptions = fixture_descriptions(plugins, 1);
    const char *screen = "{\"plugin\": \"viewer\", \"kind\": \"view\", \"id\": \"overview\"}";
    cJSON *first = plan_of(descriptions, screen, NULL);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(first, "calls")) == 2);
    cJSON *answers = cJSON_CreateArray();
    cJSON_AddItemToArray(answers, answer_to(call_for(first, "rows"), VIEWER_STATUS, VIEWER_STATUS_COUNT));
    cJSON *second = plan_of(descriptions, screen, answers);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(second, "calls")) == 1);
    CHECK(call_for(second, "greeting") != NULL);
    CHECK(cJSON_GetObjectItemCaseSensitive(second, "done") == NULL);
    cJSON *settings = plan_of(descriptions, "{\"plugin\": \"viewer\", \"kind\": \"settings\", \"id\": \"settings\"}", NULL);
    check_json(cJSON_Duplicate(settings, 1), "{\"done\": true}", "a screen binding nothing");
    cJSON *unbound = plan_of(descriptions, "{\"plugin\": \"viewer\", \"kind\": \"view\", \"id\": \"settings\"}", NULL);
    check_json(cJSON_Duplicate(unbound, 1), "{\"done\": true}", "a view binding nothing");
    cJSON_Delete(first);
    cJSON_Delete(second);
    cJSON_Delete(settings);
    cJSON_Delete(unbound);
    cJSON_Delete(answers);
    cJSON_Delete(descriptions);
}

static const char *ECHO =
    "{\"id\": \"echo\", \"api\": 1, \"services\": {\"provides\": [\"echo:main\"]}, \"views\": {\"pair\": {\"title\": \"Pair\", "
    "\"nodes\": [{\"id\": \"left\", \"component\": \"text\", \"bind\": {\"service\": \"echo:main\", \"method\": \"say\", "
    "\"argument\": {\"word\": \"left\", \"loud\": true}}},"
    "{\"id\": \"right\", \"component\": \"text\", \"bind\": {\"service\": \"echo:main\", \"method\": \"say\", "
    "\"argument\": {\"word\": \"right\"}}},"
    "{\"id\": \"again\", \"component\": \"text\", \"bind\": {\"service\": \"echo:main\", \"method\": \"say\", "
    "\"argument\": {\"loud\": true, \"word\": \"left\"}}}]}}}";

static cJSON *echo_answer(const cJSON *call) {
    cJSON *answer = answer_to(call, (const binding_fixture[]){{"say", "{\"answer\": {}}", NULL, NULL}}, 1);
    const char *word = text_at(cJSON_GetObjectItemCaseSensitive(call, "arguments"), "word");
    cJSON_AddStringToObject(cJSON_GetObjectItemCaseSensitive(answer, "answer"), "text", word);
    return answer;
}

static void tells_bindings_apart_by_their_arguments_and_shares_one_whose_arguments_are_equal(void) {
    cJSON *descriptions = cJSON_CreateArray();
    cJSON_AddItemToArray(descriptions, parsed(ECHO));
    const char *screen = "{\"plugin\": \"echo\", \"kind\": \"view\", \"id\": \"pair\"}";
    cJSON *plan = plan_of(descriptions, screen, NULL);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(plan, "calls")) == 2);
    cJSON *answers = cJSON_CreateArray();
    const cJSON *call;
    cJSON_ArrayForEach(call, cJSON_GetObjectItemCaseSensitive(plan, "calls")) {
        cJSON_AddItemToArray(answers, echo_answer(call));
    }
    cJSON *tree = successful(runtime_ui_render, screen_request(descriptions, screen, answers), "ui.render");
    check_json_at(node_of(tree, "left"), "data", "{\"text\": \"left\"}", "the left node");
    check_json_at(node_of(tree, "right"), "data", "{\"text\": \"right\"}", "the right node");
    check_json_at(node_of(tree, "again"), "data", "{\"text\": \"left\"}", "the node sharing left's call");
    cJSON_Delete(tree);
    cJSON_Delete(answers);
    cJSON_Delete(plan);
    cJSON_Delete(descriptions);
}

static void expect_render_refused(const char *screen, cJSON *answers, const char *fragment, const char *label) {
    const char *plugins[] = {"viewer"};
    cJSON *descriptions = fixture_descriptions(plugins, 1);
    catalog_answer answer = call_catalog(runtime_ui_render, screen_request(descriptions, screen, answers));
    expect_refusal(answer, fragment, label);
    cJSON_Delete(answer.tree);
    cJSON_Delete(answers);
    cJSON_Delete(descriptions);
}

static cJSON *answers_for(const char *screen) {
    const char *plugins[] = {"viewer"};
    cJSON *descriptions = fixture_descriptions(plugins, 1);
    cJSON *plan = plan_of(descriptions, screen, NULL);
    cJSON *answers = answers_to(plan, VIEWER_STATUS, VIEWER_STATUS_COUNT);
    cJSON_Delete(plan);
    cJSON_Delete(descriptions);
    return answers;
}

static const char *OVERVIEW = "{\"plugin\": \"viewer\", \"kind\": \"view\", \"id\": \"overview\"}";
static const char *CHARTS = "{\"plugin\": \"viewer\", \"kind\": \"view\", \"id\": \"charts\"}";

static void refuses_to_render_a_screen_missing_an_answer_or_one_of_its_checks(void) {
    cJSON *missing = answers_for(OVERVIEW);
    cJSON_DeleteItemFromArray(missing, 1);
    expect_render_refused(OVERVIEW, missing, "viewer:status.greeting", "a missing answer");
    expect_render_refused(OVERVIEW, NULL, "answers", "no answers at all");
    cJSON *unchecked = answers_for(OVERVIEW);
    cJSON_DeleteItemFromObjectCaseSensitive(cJSON_GetArrayItem(unchecked, 0), "checked");
    expect_render_refused(OVERVIEW, unchecked, "table", "an answer never checked");
    cJSON *whole = answers_for(CHARTS);
    cJSON *answer;
    cJSON_ArrayForEach(answer, whole) {
        cJSON *check;
        cJSON_ArrayForEach(check, cJSON_GetObjectItemCaseSensitive(answer, "checked")) {
            cJSON_DeleteItemFromObjectCaseSensitive(check, "members");
        }
    }
    expect_render_refused(CHARTS, whole, "chart", "a chart checked against the whole answer");
}

static void refuses_an_answer_with_no_outcome_or_two(void) {
    cJSON *none = answers_for(OVERVIEW);
    cJSON_DeleteItemFromObjectCaseSensitive(cJSON_GetArrayItem(none, 0), "answer");
    cJSON_DeleteItemFromObjectCaseSensitive(cJSON_GetArrayItem(none, 0), "checked");
    expect_render_refused(OVERVIEW, none, "outcome", "no outcome");
    cJSON *two = answers_for(OVERVIEW);
    cJSON_AddTrueToObject(cJSON_GetArrayItem(two, 0), "unavailable");
    expect_render_refused(OVERVIEW, two, "outcome", "two outcomes");
    cJSON *keyless = answers_for(OVERVIEW);
    cJSON_DeleteItemFromObjectCaseSensitive(cJSON_GetArrayItem(keyless, 0), "arguments");
    expect_render_refused(OVERVIEW, keyless, "answers", "no arguments");
}

static const char *LISTER =
    "{\"id\": \"lister\", \"api\": 1, \"services\": {\"provides\": [\"lister:rows\"]}, \"views\": {\"all\": {\"title\": \"All\", "
    "\"nodes\": [{\"id\": \"rows\", \"component\": \"list\", \"bind\": {\"service\": \"lister:rows\", \"method\": \"read\"}}]}}}";

static cJSON *listed(const char *rows) {
    char outcome[1024];
    snprintf(outcome, sizeof outcome, "{\"answer\": {\"items\": %s}}", rows);
    binding_fixture fixture[] = {{"read", outcome, NULL, NULL}};
    cJSON *descriptions = cJSON_CreateArray();
    cJSON_AddItemToArray(descriptions, parsed(LISTER));
    cJSON *screen = rendered_with(descriptions, "{\"plugin\": \"lister\", \"kind\": \"view\", \"id\": \"all\"}", fixture, 1);
    cJSON *presented = cJSON_Duplicate(value_at(node_of(screen, "rows"), "data/items"), 1);
    cJSON_Delete(screen);
    return presented;
}

static void marks_detected_and_available_items_unavailable_and_installable_and_leaves_installed_alone(void) {
    check_json(listed("[{\"id\": \"i\", \"state\": \"installed\"}, {\"id\": \"d\", \"state\": \"detected\"},"
                      " {\"id\": \"a\", \"state\": \"available\"}, {\"id\": \"n\"}]"),
               "[{\"id\": \"i\", \"state\": \"installed\"}, {\"id\": \"d\", \"state\": \"detected\", \"unavailable\": true,"
               " \"install\": true}, {\"id\": \"a\", \"state\": \"available\", \"unavailable\": true, \"install\": true},"
               " {\"id\": \"n\"}]",
               "presented items");
}

static void drops_an_action_a_detected_row_carried_since_an_unusable_row_offers_only_install(void) {
    check_json(listed("[{\"id\": \"d\", \"state\": \"detected\", \"values\": {}, \"action\": \"open\"},"
                      " {\"id\": \"i\", \"state\": \"installed\", \"action\": \"open\"}]"),
               "[{\"id\": \"d\", \"state\": \"detected\", \"values\": {}, \"install\": true, \"unavailable\": true},"
               " {\"id\": \"i\", \"state\": \"installed\", \"action\": \"open\"}]",
               "a dropped action");
}

static cJSON *plugin_list_screen(const char *detected) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddItemToObject(request, "installed", parsed("[{\"id\": \"viewer\"}]"));
    cJSON_AddItemToObject(request, "detected", parsed(detected));
    cJSON_AddItemToObject(request, "available", parsed("[{\"id\": \"viewer\"}, {\"id\": \"greeter\"}, {\"id\": \"rival\"}]"));
    return successful(runtime_ui_plugin_list, request, "ui.pluginList");
}

static const cJSON *row_of(const cJSON *screen, const char *id) {
    const cJSON *row;
    cJSON_ArrayForEach(row, value_at(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(screen, "nodes"), 0), "data/items")) {
        if (strcmp(text_at(row, "id"), id) == 0) {
            return row;
        }
    }
    return NULL;
}

static void shows_a_bundled_not_enabled_plugin_that_a_detector_finds_as_a_detected_installable_row(void) {
    cJSON *screen = plugin_list_screen("[{\"id\": \"greeter\"}]");
    check_text(text_at(screen, "kind"), "host", "the kind");
    check_text(text_at(screen, "title"), "Plugins", "the title");
    CHECK(cJSON_GetObjectItemCaseSensitive(screen, "plugin") == NULL);
    const cJSON *nodes = cJSON_GetObjectItemCaseSensitive(screen, "nodes");
    CHECK(cJSON_GetArraySize(nodes) == 1);
    const cJSON *table = cJSON_GetArrayItem(nodes, 0);
    check_text(text_at(table, "id"), "plugins", "the node id");
    check_text(text_at(table, "component"), "table", "the component");
    check_text(text_at(table, "state"), "ready", "the state");
    check_json_at(table, "props", "{}", "the props");
    check_json(cJSON_Duplicate(row_of(screen, "greeter"), 1),
               "{\"id\": \"greeter\", \"state\": \"detected\", \"unavailable\": true, \"install\": true}", "the greeter row");
    check_json(cJSON_Duplicate(row_of(screen, "viewer"), 1), "{\"id\": \"viewer\", \"state\": \"installed\"}", "the viewer row");
    cJSON_Delete(screen);
}

static void shows_the_same_plugin_as_available_when_no_detector_finds_it(void) {
    cJSON *screen = plugin_list_screen("[]");
    check_json(cJSON_Duplicate(row_of(screen, "greeter"), 1),
               "{\"id\": \"greeter\", \"state\": \"available\", \"unavailable\": true, \"install\": true}", "the greeter row");
    cJSON_Delete(screen);
}

static void refuses_a_plugin_list_group_that_is_no_list_of_rows(void) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddItemToObject(request, "installed", parsed("[]"));
    cJSON_AddItemToObject(request, "detected", parsed("[\"greeter\"]"));
    cJSON_AddItemToObject(request, "available", parsed("[]"));
    catalog_answer answer = call_catalog(runtime_ui_plugin_list, request);
    expect_refusal(answer, "detected", "a group of strings");
    cJSON_Delete(answer.tree);
}

int main(void) {
    renders_a_bound_table_ready_with_what_its_binding_answered();
    puts_only_the_node_whose_answer_has_the_wrong_shape_in_error_with_the_path_at_fault();
    puts_a_node_answering_a_bare_string_in_error();
    puts_a_node_whose_binding_failed_in_error_and_renders_its_sibling();
    puts_a_node_whose_binding_never_answered_in_error();
    reads_the_branch_the_nodes_chart_names_never_the_one_the_answer_carries();
    checks_a_chart_answer_against_the_branch_its_node_selects();
    carries_only_the_known_node_members_in_props();
    renders_a_bound_node_unavailable_when_its_plugin_is_not_running();
    renders_the_generated_settings_screen_through_the_same_path_as_a_view();
    renders_a_declared_view_named_settings_as_itself_and_keeps_the_generated_screen_reachable();
    refuses_to_render_an_action_entry_as_a_screen();
    puts_a_node_in_error_not_ready_with_another_plugins_answer_when_two_provide_its_service();
    renders_a_quarantined_plugins_node_unavailable_while_another_provides_the_same_service();
    refuses_a_view_the_plugin_does_not_declare();
    refuses_a_declared_view_that_carries_no_nodes_or_a_node_with_no_id();
    refuses_a_screen_of_a_plugin_no_description_names();
    gives_a_plugins_own_detected_row_the_unavailable_treatment_in_every_items_family();
    plans_each_binding_once_with_every_check_its_nodes_need();
    plans_only_the_bindings_still_unanswered_and_is_done_for_a_screen_binding_nothing();
    tells_bindings_apart_by_their_arguments_and_shares_one_whose_arguments_are_equal();
    refuses_to_render_a_screen_missing_an_answer_or_one_of_its_checks();
    refuses_an_answer_with_no_outcome_or_two();
    marks_detected_and_available_items_unavailable_and_installable_and_leaves_installed_alone();
    drops_an_action_a_detected_row_carried_since_an_unusable_row_offers_only_install();
    shows_a_bundled_not_enabled_plugin_that_a_detector_finds_as_a_detected_installable_row();
    shows_the_same_plugin_as_available_when_no_detector_finds_it();
    refuses_a_plugin_list_group_that_is_no_list_of_rows();
    return check_summary("ui_render_test");
}
