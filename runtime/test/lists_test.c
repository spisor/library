#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog_call.h"
#include "check.h"
#include "definition_fixture.h"
#include "host_provenance.h"
#include "json_value.h"
#include "registry.h"
#include "registry_call.h"
#include "runtime/lists.h"
#include "service_scratch.h"

static const char *const STATE_NAMES[] = {"(none)", "installed", "detected", "available"};

static const runtime_lists_definition *lists(void) {
    return runtime_lists_definition_of();
}

/** The list as JSON a test compares: each row as the caller gave it, or its id and label, with its state. */
static cJSON *list_json(const runtime_list *list) {
    cJSON *rows = cJSON_CreateArray();
    for (size_t index = 0; index < list->item_count; index++) {
        const runtime_list_item *item = &list->items[index];
        cJSON *row = NULL;
        if (item->row != NULL) {
            CHECK(spisor_json_of_value(item->row, &row) == SPISOR_OK);
        } else {
            row = cJSON_CreateObject();
            cJSON_AddStringToObject(row, "id", item->id);
            if (item->label != NULL) {
                cJSON_AddStringToObject(row, "label", item->label);
            }
        }
        CHECK(item->id != NULL && strcmp(item->id, cJSON_GetStringValue(cJSON_GetObjectItem(row, "id"))) == 0);
        cJSON_AddStringToObject(row, "state", STATE_NAMES[item->state <= 3 ? item->state : 0]);
        cJSON_AddItemToArray(rows, row);
    }
    return rows;
}

/** What plugins answers for the three groups, given as JSON text, with its refusal. */
static spisor_status plugins_through(const char *installed, const char *detected, const char *available,
                                     runtime_list *answer, spisor_refusal *refusal) {
    spisor_value_tree groups[3] = {SPISOR_INIT(spisor_value_tree), SPISOR_INIT(spisor_value_tree),
                                   SPISOR_INIT(spisor_value_tree)};
    runtime_lists_plugins_argument argument = SPISOR_INIT(runtime_lists_plugins_argument);
    CHECK(value_of_text(installed, &groups[0]) && value_of_text(detected, &groups[1]) &&
          value_of_text(available, &groups[2]));
    argument.installed = &groups[0].root;
    argument.detected = &groups[1].root;
    argument.available = &groups[2].root;
    spisor_status status = lists()->plugins(lists()->context, &argument, answer, refusal);
    for (size_t index = 0; index < 3; index++) {
        spisor_value_release(&groups[index]);
    }
    return status;
}

/** The rows plugins answers for the three groups, given as JSON text. */
static cJSON *plugin_rows(const char *installed, const char *detected, const char *available) {
    runtime_list answer = SPISOR_INIT(runtime_list);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    CHECK(plugins_through(installed, detected, available, &answer, &refusal) == SPISOR_OK);
    cJSON *items = list_json(&answer);
    lists()->release_list(lists()->context, &answer);
    lists()->release_refusal(lists()->context, &refusal);
    return items;
}

static void check_json(cJSON *actual, const char *expected, const char *label) {
    cJSON *wanted = parsed(expected);
    if (!cJSON_Compare(actual, wanted, 1)) {
        char *text = actual == NULL ? NULL : cJSON_PrintUnformatted(actual);
        fprintf(stderr, "%s: expected %s, got %s\n", label, expected, text == NULL ? "(null)" : text);
        cJSON_free(text);
        check_failures++;
    }
    cJSON_Delete(wanted);
    cJSON_Delete(actual);
}

static void lists_an_id_found_in_all_three_once_as_installed(void) {
    check_json(plugin_rows("[{\"id\": \"a\"}]", "[{\"id\": \"a\"}]", "[{\"id\": \"a\"}]"),
               "[{\"id\": \"a\", \"state\": \"installed\"}]", "all three");
}

static void lists_an_id_found_as_detected_and_available_as_detected(void) {
    check_json(plugin_rows("[]", "[{\"id\": \"a\"}]", "[{\"id\": \"a\"}]"), "[{\"id\": \"a\", \"state\": \"detected\"}]",
               "detected and available");
}

static void keeps_the_first_of_an_id_repeated_within_one_input(void) {
    check_json(plugin_rows("[{\"id\": \"a\", \"label\": \"first\"}, {\"id\": \"a\", \"label\": \"second\"}]", "[]", "[]"),
               "[{\"id\": \"a\", \"label\": \"first\", \"state\": \"installed\"}]", "a repeated id");
}

static void orders_by_state_then_by_id(void) {
    cJSON *rows = plugin_rows("[{\"id\": \"z\"}, {\"id\": \"b\"}]", "[{\"id\": \"y\"}, {\"id\": \"c\"}]",
                              "[{\"id\": \"x\"}, {\"id\": \"a\"}]");
    char order[256] = "";
    const cJSON *row;
    cJSON_ArrayForEach(row, rows) {
        char entry[64];
        snprintf(entry, sizeof entry, "%s%s:%s", order[0] == '\0' ? "" : ",",
                 cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "state")),
                 cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "id")));
        strncat(order, entry, sizeof order - strlen(order) - 1);
    }
    check_text(order, "installed:b,installed:z,detected:c,detected:y,available:a,available:x", "the order");
    cJSON_Delete(rows);
}

static void answers_an_enabled_plugin_installed_a_detected_one_detected_and_the_rest_available(void) {
    check_json(plugin_rows("[{\"id\": \"viewer\"}]", "[{\"id\": \"greeter\"}]",
                           "[{\"id\": \"viewer\"}, {\"id\": \"greeter\"}, {\"id\": \"rival\"}]"),
               "[{\"id\": \"viewer\", \"state\": \"installed\"}, {\"id\": \"greeter\", \"state\": \"detected\"},"
               " {\"id\": \"rival\", \"state\": \"available\"}]",
               "plugins");
}

static void refuses_a_group_that_is_no_list_of_rows_with_an_id(void) {
    const char *groups[] = {"{}", "[{\"label\": \"x\"}]", "[\"a\"]"};
    for (size_t index = 0; index < sizeof groups / sizeof groups[0]; index++) {
        runtime_list answer = SPISOR_INIT(runtime_list);
        spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
        check_refused(plugins_through("[]", groups[index], "[]", &answer, &refusal), SPISOR_MALFORMED, &refusal,
                      "list of rows", groups[index]);
        CHECK(answer.storage == NULL);
        lists()->release_refusal(lists()->context, &refusal);
    }
}

static const char *CATALOG =
    "{\"format\": 1, \"items\": {"
    "\"claude-host\": {\"kind\": \"host\", \"versions\": {}, \"latest\": \"1.0.0\", \"application\": {\"id\": \"claude\","
    " \"label\": \"claude label\", \"binary\": \"claude\", \"home\": {\"candidates\": [\"~/.x\"]}, \"packages\": {}}},"
    "\"opencode-host\": {\"kind\": \"host\", \"versions\": {}, \"latest\": \"1.0.0\", \"application\": {\"id\": "
    "\"opencode\", \"label\": \"opencode label\", \"binary\": \"opencode\", \"home\": {\"candidates\": [\"~/.x\"]},"
    " \"packages\": {}}},"
    "\"gemini-host\": {\"kind\": \"host\", \"versions\": {}, \"latest\": \"1.0.0\", \"application\": {\"id\": \"gemini\","
    " \"label\": \"gemini label\", \"binary\": \"gemini\", \"home\": {\"candidates\": [\"~/.x\"]}, \"packages\": {}}},"
    "\"some-plugin\": {\"kind\": \"plugin\", \"versions\": {}, \"latest\": \"1.0.0\"}}}";

typedef struct lists_scratch {
    char base[1024];
    char *state;
    char *bin;
} lists_scratch;

static void fresh(lists_scratch *place, const char *label) {
    scratch_in(place->base, sizeof place->base, "lists", label);
    place->state = joined_path(place->base, "state");
    place->bin = joined_path(place->base, "bin");
    CHECK(spisor_make_directories(place->state) == 1 && spisor_make_directories(place->bin) == 1);
}

static void discard(lists_scratch *place) {
    free(place->state);
    free(place->bin);
}

static void register_claude_host(const lists_scratch *place) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(cJSON_AddObjectToObject(request, "places"), "state", place->state);
    cJSON *registration = cJSON_AddObjectToObject(request, "registration");
    cJSON_AddStringToObject(registration, "id", "claude-host");
    cJSON_AddStringToObject(registration, "kind", "host");
    cJSON_AddStringToObject(registration, "version", "1.0.0");
    cJSON_AddItemToObject(registration, "application",
                          parsed("{\"id\": \"claude\", \"label\": \"claude label\", \"binary\": \"claude\","
                                 " \"home\": {\"candidates\": [\"~/.x\"]}, \"packages\": {}}"));
    cJSON_AddStringToObject(registration, "home", place->base);
    add_host_provenance(registration, "test", "1.0.0", place->base);
    cJSON_AddFalseToObject(registration, "requiresCommand");
    registry_result result = registry_call(REGISTRY_REGISTER, request);
    CHECK(result.status == SPISOR_OK);
    cJSON_Delete(result.tree);
}

/** What applications answers for the catalog, the environment and the detections, each written as test data. */
static spisor_status applications_through(const char *state, const char *catalog, cJSON *environment,
                                          const char *detections_json, runtime_list *answer, spisor_refusal *refusal) {
    cJSON *catalog_json = parsed(catalog);
    cJSON *detections = parsed(detections_json);
    spisor_value_tree items = SPISOR_INIT(spisor_value_tree);
    runtime_variable *variables = calloc((size_t)cJSON_GetArraySize(environment) + 1, sizeof *variables);
    runtime_list_detection *read = calloc((size_t)cJSON_GetArraySize(detections) + 1, sizeof *read);
    runtime_lists_applications_argument argument = SPISOR_INIT(runtime_lists_applications_argument);
    const cJSON *member;
    CHECK(spisor_vector_value(catalog_json, "items", &items));
    cJSON_ArrayForEach(member, environment) {
        variables[argument.environment_count].name = member->string;
        variables[argument.environment_count++].value = cJSON_GetStringValue(member);
    }
    cJSON_ArrayForEach(member, detections) {
        runtime_list_detection *detection = &read[argument.detection_count++];
        detection->item = cJSON_GetStringValue(cJSON_GetObjectItem(member, "item"));
        detection->installed = cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(member, "answer"), "installed"));
        detection->refused = cJSON_GetStringValue(cJSON_GetObjectItem(member, "refused"));
    }
    argument.items = &items.root;
    argument.state = state;
    argument.environment = variables;
    argument.detections = read;
    spisor_status status = lists()->applications(lists()->context, &argument, answer, refusal);
    spisor_value_release(&items);
    free(variables);
    free(read);
    cJSON_Delete(detections);
    cJSON_Delete(environment);
    cJSON_Delete(catalog_json);
    return status;
}

static cJSON *application_rows(const lists_scratch *place, cJSON *environment, const char *detections) {
    runtime_list answer = SPISOR_INIT(runtime_list);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    CHECK(applications_through(place->state, CATALOG, environment, detections, &answer, &refusal) == SPISOR_OK);
    cJSON *items = list_json(&answer);
    lists()->release_list(lists()->context, &answer);
    lists()->release_refusal(lists()->context, &refusal);
    return items;
}

#if defined(_WIN32)
#define PROGRAM_EXTENSION ".cmd"
#else
#define PROGRAM_EXTENSION ""
#endif

static cJSON *path_environment(const char *search) {
    cJSON *environment = cJSON_CreateObject();
    cJSON_AddStringToObject(environment, "PATH", search);
    cJSON_AddStringToObject(environment, "PATHEXT", ".CMD");
    return environment;
}

static const char *state_of(const cJSON *rows, const char *id) {
    const cJSON *row;
    cJSON_ArrayForEach(row, rows) {
        const char *row_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "id"));
        if (row_id != NULL && strcmp(row_id, id) == 0) {
            return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "state"));
        }
    }
    return NULL;
}

static void lists_installed_from_the_registry_detected_from_path_or_a_method_and_the_rest_available(void) {
    lists_scratch place;
    fresh(&place, "applications");
    register_claude_host(&place);
    free(touch_file(place.bin, "opencode" PROGRAM_EXTENSION, "", 0755));
    check_json(application_rows(&place, path_environment(place.bin), "[]"),
               "[{\"id\": \"claude\", \"label\": \"claude label\", \"state\": \"installed\"},"
               " {\"id\": \"opencode\", \"label\": \"opencode label\", \"state\": \"detected\"},"
               " {\"id\": \"gemini\", \"label\": \"gemini label\", \"state\": \"available\"}]",
               "applications");
    discard(&place);
}

static void lists_an_application_detected_when_a_method_reports_its_package_installed(void) {
    lists_scratch place;
    fresh(&place, "method");
    cJSON *rows = application_rows(
        &place, path_environment(""),
        "[{\"method\": \"npm\", \"item\": \"gemini\", \"package\": \"@x/gemini\", \"answer\": {\"installed\": true}}]");
    check_text(state_of(rows, "gemini"), "detected", "gemini");
    cJSON_Delete(rows);
    discard(&place);
}

#if defined(_WIN32)
#define MARKING_PROGRAM_FORMAT "@echo ran> \"%s\"\r\n"
#else
#define MARKING_PROGRAM_FORMAT "#!/bin/sh\necho ran > '%s'\n"
#endif

static void detects_a_binary_on_path_without_launching_anything(void) {
    lists_scratch place;
    fresh(&place, "launch");
    char *marker = joined_path(place.base, "ran");
    char program[2048];
    snprintf(program, sizeof program, MARKING_PROGRAM_FORMAT, marker);
    free(touch_file(place.bin, "claude" PROGRAM_EXTENSION, program, 0755));
    cJSON *rows = application_rows(&place, path_environment(place.bin), "[]");
    check_text(state_of(rows, "claude"), "detected", "claude on PATH");
    CHECK(!spisor_file_exists(marker));
    cJSON_Delete(rows);
    free(marker);
    discard(&place);
}

static void ignores_a_method_answer_about_another_item_or_reporting_not_installed(void) {
    lists_scratch place;
    fresh(&place, "other");
    cJSON *rows = application_rows(
        &place, path_environment(place.bin),
        "[{\"method\": \"npm\", \"item\": \"other\", \"package\": \"@x/claude\", \"answer\": {\"installed\": true}},"
        " {\"method\": \"npm\", \"item\": \"claude\", \"package\": \"@x/claude\", \"answer\": {\"installed\": false}}]");
    check_text(state_of(rows, "claude"), "available", "claude");
    cJSON_Delete(rows);
    discard(&place);
}

static void never_detects_from_a_refused_method(void) {
    lists_scratch place;
    fresh(&place, "refused");
    cJSON *rows = application_rows(
        &place, path_environment(place.bin),
        "[{\"method\": \"npm\", \"item\": \"claude\", \"refused\": \"npm is not installed\","
        " \"answer\": {\"installed\": true}}]");
    check_text(state_of(rows, "claude"), "available", "claude");
    cJSON_Delete(rows);
    discard(&place);
}

static void refuses_items_that_are_no_record_or_a_detection_without_its_item(void) {
    lists_scratch place;
    fresh(&place, "shape");
    runtime_list answer = SPISOR_INIT(runtime_list);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    check_refused(applications_through(place.state, "{\"format\": 1, \"items\": [1]}", path_environment(""), "[]",
                                       &answer, &refusal),
                  SPISOR_MALFORMED, &refusal, "not a record", "items that are a list");
    lists()->release_refusal(lists()->context, &refusal);
    check_refused(applications_through(place.state, CATALOG, path_environment(""), "[{\"method\": \"npm\"}]", &answer,
                                       &refusal),
                  SPISOR_MALFORMED, &refusal, "detection", "a detection without its item");
    lists()->release_refusal(lists()->context, &refusal);
    check_refused(applications_through("relative/state", CATALOG, path_environment(""), "[]", &answer, &refusal),
                  SPISOR_MALFORMED, &refusal, "state", "a relative state place");
    lists()->release_refusal(lists()->context, &refusal);
    CHECK(answer.storage == NULL);
    discard(&place);
}

int main(void) {
    lists_an_id_found_in_all_three_once_as_installed();
    lists_an_id_found_as_detected_and_available_as_detected();
    keeps_the_first_of_an_id_repeated_within_one_input();
    orders_by_state_then_by_id();
    answers_an_enabled_plugin_installed_a_detected_one_detected_and_the_rest_available();
    refuses_a_group_that_is_no_list_of_rows_with_an_id();
    lists_installed_from_the_registry_detected_from_path_or_a_method_and_the_rest_available();
    lists_an_application_detected_when_a_method_reports_its_package_installed();
    detects_a_binary_on_path_without_launching_anything();
    ignores_a_method_answer_about_another_item_or_reporting_not_installed();
    never_detects_from_a_refused_method();
    refuses_items_that_are_no_record_or_a_detection_without_its_item();
    return check_summary("lists_test");
}
