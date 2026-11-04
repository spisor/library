#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog.h"
#include "catalog_call.h"
#include "check.h"
#include "config.h"
#include "scratch_directory.h"
#include "spisor/json_kind.h"

#if defined(_WIN32)
#include <process.h>
#define current_process_id() _getpid()
#else
#include <unistd.h>
#define current_process_id() getpid()
#endif

#define ITEM_ONE \
    "{\"kind\":\"plugin\",\"latest\":\"1\",\"versions\":{\"1\":{\"files\":{\"p.js\":{\"url\":\"https://e.com/p.js\"}}}}}"
#define ITEM_TWO \
    "{\"kind\":\"plugin\",\"latest\":\"2\",\"versions\":{\"2\":{\"files\":{\"p.js\":{\"url\":\"https://e.com/p.js\"}}}}}"
#define FIRST "{\"kind\":\"local\",\"path\":\"a\"}"
#define SECOND "{\"kind\":\"https\",\"url\":\"https://e.com/i.json\"}"

static catalog_answer sources_of(const char *settings_json) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "settings", parsed(settings_json));
    return call_catalog(runtime_catalog_sources, argument);
}

static void expect_json(const cJSON *actual, const char *expected_json, const char *label) {
    cJSON *expected = parsed(expected_json);
    if (actual == NULL || !cJSON_Compare(actual, expected, 1)) {
        char *printed = actual == NULL ? NULL : cJSON_PrintUnformatted(actual);
        fprintf(stderr, "%s: expected %s, got %s\n", label, expected_json, printed == NULL ? "(none)" : printed);
        cJSON_free(printed);
        check_failures++;
    }
    cJSON_Delete(expected);
}

static void it_reads_local_and_https_sources_in_order(void) {
    catalog_answer answer = sources_of("{\"catalog\":{\"sources\":[{\"kind\":\"local\",\"path\":\"C:/out\"},"
                                       "{\"kind\":\"https\",\"url\":\"https://e.com/index.json\"}]}}");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    expect_json(value_at(answer.tree, "sources"),
                "[{\"kind\":\"local\",\"path\":\"C:/out\"},{\"kind\":\"https\",\"url\":\"https://e.com/index.json\"}]",
                "in order");
    expect_json(value_at(answer.tree, "skipped"), "[]", "in order");
    cJSON_Delete(answer.tree);
}

static int32_t settings_call(catalog_function function, const char *home, const char *portable, cJSON *argument,
                             cJSON **tree) {
    cJSON_AddStringToObject(argument, "home", home);
    cJSON_AddStringToObject(argument, "portable", portable);
    catalog_answer answer = call_catalog(function, argument);
    *tree = answer.tree;
    return answer.status;
}

static void it_reads_the_sources_a_real_settings_round_trip_stores(void) {
    char base[1024];
    char home[1100];
    char portable[1100];
    snprintf(base, sizeof base, "%s/catalog-sources-%d", RUNTIME_SCRATCH_PATH, (int)current_process_id());
    snprintf(home, sizeof home, "%s/home", base);
    snprintf(portable, sizeof portable, "%s/portable", base);
    start_empty_directory(base);
    const char *sources = "[{\"kind\":\"https\",\"url\":\"https://e.com/index.json\"}]";
    cJSON *set = cJSON_CreateObject();
    cJSON_AddStringToObject(set, "key", "catalog.sources");
    cJSON_AddItemToObject(set, "value", parsed(sources));
    cJSON_AddStringToObject(set, "layer", "portable");
    cJSON *ignored;
    CHECK(settings_call(runtime_settings_set, home, portable, set, &ignored) == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(ignored);
    cJSON *all;
    CHECK(settings_call(runtime_settings_all, home, portable, cJSON_CreateObject(), &all) == SPISOR_STATUS_SUCCESS);
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "settings", cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(all, "values"), 1));
    cJSON_Delete(all);
    catalog_answer answer = call_catalog(runtime_catalog_sources, argument);
    expect_json(value_at(answer.tree, "sources"), sources, "round trip");
    cJSON_Delete(answer.tree);
    CHECK(runtime_remove_tree(base) == 1);
}

static void it_is_empty_without_the_setting(void) {
    catalog_answer answer = sources_of("{}");
    expect_json(answer.tree, "{\"sources\":[],\"skipped\":[]}", "no setting");
    cJSON_Delete(answer.tree);
}

static void it_skips_an_unknown_kind_and_reports_it(void) {
    catalog_answer answer = sources_of("{\"catalog\":{\"sources\":[{\"kind\":\"ftp\",\"host\":\"x\"},{\"kind\":\"local\","
                                       "\"path\":\"p\"},\"junk\",{\"kind\":\"https\"}]}}");
    expect_json(value_at(answer.tree, "sources"), "[{\"kind\":\"local\",\"path\":\"p\"}]", "skips");
    const cJSON *skipped = value_at(answer.tree, "skipped");
    int indexes[] = {0, 2, 3};
    int count = 0;
    for (const cJSON *entry = skipped == NULL ? NULL : skipped->child; entry != NULL; entry = entry->next, count++) {
        const cJSON *index = cJSON_GetObjectItemCaseSensitive(entry, "index");
        CHECK(count < 3 && cJSON_IsNumber(index) && index->valueint == indexes[count]);
    }
    CHECK(count == 3);
    const char *first = text_at(skipped == NULL ? NULL : skipped->child, "detail");
    CHECK(first != NULL && strstr(first, "ftp") != NULL);
    check_text(text_at(cJSON_GetArrayItem(skipped, 1), "detail"), "a catalog source must be an object with a kind",
               "not an object");
    check_text(text_at(cJSON_GetArrayItem(skipped, 2), "detail"), "unknown or incomplete catalog source kind \"https\"",
               "incomplete");
    cJSON_Delete(answer.tree);
}

static void it_refuses_settings_that_are_not_an_object(void) {
    expect_refusal(call_catalog(runtime_catalog_sources, parsed("{\"settings\":[]}")), "settings", "settings array");
}

static catalog_answer merged(const char *catalogs_json) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "catalogs", parsed(catalogs_json));
    return call_catalog(runtime_catalog_merge, argument);
}

static void it_keeps_the_earlier_source_s_item_and_reports_the_later_one_shadowed(void) {
    catalog_answer answer = merged("[{\"source\":" FIRST ",\"catalog\":{\"format\":1,\"items\":{\"x\":" ITEM_ONE
                                   ",\"y\":" ITEM_ONE "}}},{\"source\":" SECOND
                                   ",\"catalog\":{\"format\":1,\"items\":{\"x\":" ITEM_TWO ",\"z\":" ITEM_TWO "}}}]");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    const cJSON *items = value_at(answer.tree, "catalog/items");
    check_text(text_at(value_at(items, "x"), "latest"), "1", "earlier wins");
    CHECK(cJSON_GetArraySize(items) == 3 && value_at(items, "y") != NULL && value_at(items, "z") != NULL);
    expect_json(value_at(answer.tree, "shadowed"), "[{\"id\":\"x\",\"source\":" SECOND "}]", "shadowed");
    cJSON_Delete(answer.tree);
}

static void it_treats_an_inherited_object_member_name_as_absent(void) {
    catalog_answer answer =
        merged("[{\"source\":" FIRST ",\"catalog\":{\"format\":1,\"items\":{\"constructor\":" ITEM_ONE "}}}]");
    expect_json(value_at(answer.tree, "shadowed"), "[]", "constructor");
    CHECK(value_at(answer.tree, "catalog/items/constructor") != NULL);
    cJSON_Delete(answer.tree);
}

static void it_merges_nothing_into_an_empty_catalog(void) {
    catalog_answer answer = merged("[]");
    expect_json(answer.tree, "{\"catalog\":{\"format\":1,\"items\":{}},\"shadowed\":[]}", "empty");
    cJSON_Delete(answer.tree);
}

static void it_merges_ids_in_the_order_javascript_enumerates_them(void) {
    catalog_answer answer =
        merged("[{\"source\":" FIRST ",\"catalog\":{\"format\":1,\"items\":{\"b\":" ITEM_ONE ",\"7\":" ITEM_ONE "}}}]");
    const cJSON *items = value_at(answer.tree, "catalog/items");
    CHECK(items != NULL && items->child != NULL && strcmp(items->child->string, "7") == 0);
    cJSON_Delete(answer.tree);
}

static void it_refuses_catalogs_it_cannot_merge(void) {
    expect_refusal(merged("[{\"source\":" FIRST ",\"catalog\":{\"format\":1}}]"), "catalogs", "no items");
    expect_refusal(merged("[{\"source\":{\"kind\":\"ftp\"},\"catalog\":{\"format\":1,\"items\":{}}}]"), "catalogs",
                   "bad source");
}

int main(void) {
    it_reads_local_and_https_sources_in_order();
    it_reads_the_sources_a_real_settings_round_trip_stores();
    it_is_empty_without_the_setting();
    it_skips_an_unknown_kind_and_reports_it();
    it_refuses_settings_that_are_not_an_object();
    it_keeps_the_earlier_source_s_item_and_reports_the_later_one_shadowed();
    it_treats_an_inherited_object_member_name_as_absent();
    it_merges_nothing_into_an_empty_catalog();
    it_merges_ids_in_the_order_javascript_enumerates_them();
    it_refuses_catalogs_it_cannot_merge();
    return check_summary("catalog_sources_test");
}
