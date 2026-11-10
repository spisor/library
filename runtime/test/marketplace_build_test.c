#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog_call.h"
#include "check.h"
#include "fetch_hooks.h"
#include "http_fixture.h"
#include "marketplace_build.h"
#include "marketplace_check.h"

#define BUILT "2026-11-10T12:00:00Z"
#define REPOSITORY(identifier, name_text, full_name_text) \
    "{\"id\":" identifier ",\"name\":\"" name_text "\",\"full_name\":\"" full_name_text "\",\"default_branch\":\"main\"}"
#define ROUTE(path, status, body) {path, status, NULL, body, sizeof body - 1, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0}
#define EMPTY_ROUTE(path, status) {path, status, NULL, "", 0, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0}
#define LIMITED_ROUTE(path, body) {path, 200, NULL, body, sizeof body - 1, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 1}

static unsigned short server_port;

static catalog_answer build_with(const char *list_json, const char *built) {
    char api_base[128];
    snprintf(api_base, sizeof api_base, "http://127.0.0.1:%u", (unsigned)server_port);
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "list", parsed(list_json));
    cJSON_AddStringToObject(argument, "built", built);
    cJSON_AddStringToObject(argument, "apiBase", api_base);
    return call_catalog(runtime_marketplace_build, argument);
}

static catalog_answer build(const char *list_json) {
    return build_with(list_json, BUILT);
}

static const cJSON *entry_at(const catalog_answer answer, int index) {
    return cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(value_at(answer.tree, "copy"), "entries"), index);
}

static int entry_member_is(const catalog_answer answer, int index, const char *member, const char *expected) {
    return check_text(text_at(entry_at(answer, index), member), expected, member);
}

static void builds_a_copy_with_current_names(void) {
    catalog_answer answer = build("{\"format\":1,\"title\":\"Spisor\",\"entries\":["
                                  "{\"id\":\"github:1\",\"description\":\"The runtime.\",\"categories\":[\"core\"]},"
                                  "{\"id\":\"github:2\"}]}");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    const cJSON *copy = value_at(answer.tree, "copy");
    CHECK(cJSON_GetObjectItemCaseSensitive(copy, "format")->valueint == 1);
    check_text(text_at(copy, "title"), "Spisor", "title");
    check_text(text_at(copy, "built"), BUILT, "built");
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(copy, "entries")) == 2);
    entry_member_is(answer, 0, "id", "github:1");
    entry_member_is(answer, 0, "name", "runtime");
    entry_member_is(answer, 0, "fullName", "spisor/runtime");
    entry_member_is(answer, 0, "lastKnown", "spisor/runtime");
    entry_member_is(answer, 0, "description", "The runtime.");
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(entry_at(answer, 0), "categories")) == 1);
    entry_member_is(answer, 1, "name", "weivel");
    CHECK(cJSON_GetObjectItemCaseSensitive(entry_at(answer, 1), "description") == NULL);
    char problem[256];
    CHECK(runtime_marketplace_check_copy(copy, problem, sizeof problem));
    cJSON_Delete(answer.tree);
}

static void a_renamed_repo_gets_its_new_name_with_no_change_to_the_list(void) {
    const char *list = "{\"format\":1,\"entries\":[{\"id\":\"github:3\"}]}";
    catalog_answer before = build(list);
    catalog_answer after = build(list);
    CHECK(before.status == SPISOR_STATUS_SUCCESS && after.status == SPISOR_STATUS_SUCCESS);
    entry_member_is(before, 0, "name", "old-name");
    entry_member_is(before, 0, "fullName", "spisor/old-name");
    entry_member_is(after, 0, "name", "new-name");
    entry_member_is(after, 0, "fullName", "spisor/new-name");
    entry_member_is(after, 0, "lastKnown", "spisor/new-name");
    cJSON_Delete(before.tree);
    cJSON_Delete(after.tree);
}

static void an_alias_replaces_the_name(void) {
    catalog_answer answer = build("{\"format\":1,\"entries\":[{\"id\":\"github:1\",\"alias\":\"core\"}]}");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    entry_member_is(answer, 0, "name", "core");
    entry_member_is(answer, 0, "fullName", "spisor/runtime");
    cJSON_Delete(answer.tree);
}

static void two_entries_with_one_name_and_no_alias_are_refused_naming_both_ids(void) {
    catalog_answer answer = build("{\"format\":1,\"entries\":[{\"id\":\"github:1\"},{\"id\":\"github:4\"}]}");
    CHECK(expect_refusal(answer, "github:1", "a clash names the first id"));
    CHECK(strstr(text_at(answer.tree, "message"), "github:4") != NULL);
    CHECK(strstr(text_at(answer.tree, "message"), "runtime") != NULL);
    CHECK(strstr(text_at(answer.tree, "fix"), "alias") != NULL);
    cJSON_Delete(answer.tree);
}

static void an_alias_settles_a_clash(void) {
    catalog_answer answer =
        build("{\"format\":1,\"entries\":[{\"id\":\"github:1\"},{\"id\":\"github:4\",\"alias\":\"other-runtime\"}]}");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    entry_member_is(answer, 0, "name", "runtime");
    entry_member_is(answer, 1, "name", "other-runtime");
    cJSON_Delete(answer.tree);
}

static void a_clash_is_checked_after_aliases_apply(void) {
    catalog_answer clash =
        build("{\"format\":1,\"entries\":[{\"id\":\"github:1\",\"alias\":\"weivel\"},{\"id\":\"github:2\"}]}");
    CHECK(expect_refusal(clash, "weivel", "an alias that equals another entry's name clashes"));
    cJSON_Delete(clash.tree);
    catalog_answer freed =
        build("{\"format\":1,\"entries\":[{\"id\":\"github:1\",\"alias\":\"core\"},{\"id\":\"github:4\"}]}");
    CHECK(freed.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(freed.tree);
}

static void a_duplicate_id_is_refused(void) {
    catalog_answer answer =
        build("{\"format\":1,\"entries\":[{\"id\":\"github:1\"},{\"id\":\"github:1\",\"alias\":\"again\"}]}");
    CHECK(expect_refusal(answer, "github:1", "a duplicate id"));
    CHECK(strstr(text_at(answer.tree, "message"), "twice") != NULL);
    cJSON_Delete(answer.tree);
}

static void a_local_id_is_refused(void) {
    catalog_answer answer = build("{\"format\":1,\"entries\":[{\"id\":\"local:tool\"}]}");
    CHECK(expect_refusal(answer, "local:tool", "a local id"));
    cJSON_Delete(answer.tree);
}

static void a_folder_id_builds_with_the_last_segment_as_its_name(void) {
    catalog_answer answer = build("{\"format\":1,\"entries\":[{\"id\":\"github:1/tools/config_ledger\"}]}");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    entry_member_is(answer, 0, "id", "github:1/tools/config_ledger");
    entry_member_is(answer, 0, "name", "config-ledger");
    entry_member_is(answer, 0, "fullName", "spisor/runtime");
    entry_member_is(answer, 0, "lastKnown", "spisor/runtime");
    cJSON_Delete(answer.tree);
}

static void two_folders_of_one_repo_build(void) {
    catalog_answer answer = build(
        "{\"format\":1,\"entries\":[{\"id\":\"github:1/first\"},{\"id\":\"github:1/second\"},{\"id\":\"github:1\"}]}");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    entry_member_is(answer, 0, "name", "first");
    entry_member_is(answer, 1, "name", "second");
    entry_member_is(answer, 2, "name", "runtime");
    cJSON_Delete(answer.tree);
}

static void an_alias_replaces_a_folders_name(void) {
    catalog_answer answer = build("{\"format\":1,\"entries\":[{\"id\":\"github:1/first\",\"alias\":\"chosen\"}]}");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    entry_member_is(answer, 0, "name", "chosen");
    cJSON_Delete(answer.tree);
}

static void two_folders_with_one_last_segment_clash(void) {
    catalog_answer answer =
        build("{\"format\":1,\"entries\":[{\"id\":\"github:1/a/same\"},{\"id\":\"github:2/b/same\"}]}");
    CHECK(expect_refusal(answer, "github:1/a/same", "a clash between folders"));
    cJSON_Delete(answer.tree);
}

static void a_local_folder_id_is_still_refused(void) {
    catalog_answer answer = build("{\"format\":1,\"entries\":[{\"id\":\"local:tool/sub\"}]}");
    CHECK(expect_refusal(answer, "local:tool/sub", "a local folder id"));
    cJSON_Delete(answer.tree);
}

static void a_host_name_is_derived_into_an_import_name(void) {
    catalog_answer answer = build("{\"format\":1,\"entries\":[{\"id\":\"github:5\"},{\"id\":\"github:9\"}]}");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    entry_member_is(answer, 0, "name", "my-runtime");
    entry_member_is(answer, 0, "fullName", "spisor/My_Runtime");
    entry_member_is(answer, 1, "name", "a.b-c");
    char problem[256];
    CHECK(runtime_marketplace_check_copy(value_at(answer.tree, "copy"), problem, sizeof problem));
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(answer.tree, "skipped")) == 0);
    cJSON_Delete(answer.tree);
}

static void an_entry_whose_name_cannot_be_derived_is_skipped_and_the_rest_built(void) {
    catalog_answer answer =
        build("{\"format\":1,\"entries\":[{\"id\":\"github:2\"},{\"id\":\"github:6\"},{\"id\":\"github:10\"}]}");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    const cJSON *entries = cJSON_GetObjectItemCaseSensitive(value_at(answer.tree, "copy"), "entries");
    CHECK(cJSON_GetArraySize(entries) == 1);
    entry_member_is(answer, 0, "id", "github:2");
    const cJSON *skipped = cJSON_GetObjectItemCaseSensitive(answer.tree, "skipped");
    CHECK(cJSON_GetArraySize(skipped) == 2);
    check_text(text_at(cJSON_GetArrayItem(skipped, 0), "id"), "github:6", "skipped id");
    CHECK(strstr(text_at(cJSON_GetArrayItem(skipped, 0), "reason"), "Con") != NULL);
    check_text(text_at(cJSON_GetArrayItem(skipped, 1), "id"), "github:10", "an empty derived name");
    cJSON_Delete(answer.tree);
}

static void an_alias_lists_an_entry_whose_name_cannot_be_derived(void) {
    catalog_answer answer = build("{\"format\":1,\"entries\":[{\"id\":\"github:6\",\"alias\":\"console\"}]}");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    entry_member_is(answer, 0, "name", "console");
    cJSON_Delete(answer.tree);
}

static void a_clash_after_derivation_refuses_the_whole_build(void) {
    catalog_answer answer = build("{\"format\":1,\"entries\":[{\"id\":\"github:5\"},{\"id\":\"github:8\"}]}");
    CHECK(expect_refusal(answer, "github:5", "a derived clash"));
    CHECK(strstr(text_at(answer.tree, "message"), "github:8") != NULL);
    CHECK(strstr(text_at(answer.tree, "message"), "my-runtime") != NULL);
    cJSON_Delete(answer.tree);
}

static void a_list_that_breaks_the_schema_is_refused(void) {
    const char *lists[] = {"{\"format\":1}", "{\"format\":2,\"entries\":[]}", "{\"format\":1,\"entries\":[{}]}",
                           "{\"format\":1,\"entries\":[{\"id\":\"Github:1\"}]}",
                           "{\"format\":1,\"entries\":[{\"id\":\"github:1\",\"name\":\"runtime\"}]}",
                           "{\"format\":1,\"entries\":[{\"id\":\"github:1\",\"alias\":\"Runtime\"}]}", "[]"};
    for (size_t index = 0; index < sizeof lists / sizeof lists[0]; index++) {
        catalog_answer answer = build(lists[index]);
        CHECK(expect_refusal(answer, "list", lists[index]));
        cJSON_Delete(answer.tree);
    }
}

static void a_built_time_that_is_not_rfc_3339_is_refused(void) {
    catalog_answer answer = build_with("{\"format\":1,\"entries\":[{\"id\":\"github:1\"}]}", "yesterday");
    CHECK(expect_refusal(answer, "built", "a built time"));
    cJSON_Delete(answer.tree);
}

static void an_empty_list_builds_an_empty_copy(void) {
    catalog_answer answer = build("{\"format\":1,\"entries\":[]}");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(value_at(answer.tree, "copy"), "entries")) == 0);
    cJSON_Delete(answer.tree);
}

#define TOKEN "tok-1234567890abcdef"
#define AUTHORIZATION_LINE "Authorization: token " TOKEN
#define KEPT_ENTRY "{\"id\":\"github:60\",\"name\":\"kept\",\"fullName\":\"spisor/kept\",\"lastKnown\":\"spisor/kept\",\"description\":\"Old.\"}"
#define PREVIOUS_COPY(entries) "{\"format\":1,\"built\":\"2026-11-01T12:00:00Z\",\"entries\":[" entries "]}"

static catalog_answer build_with_options(const char *list_json, const char *token, const char *previous_json) {
    char api_base[128];
    snprintf(api_base, sizeof api_base, "http://127.0.0.1:%u", (unsigned)server_port);
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "list", parsed(list_json));
    cJSON_AddStringToObject(argument, "built", BUILT);
    cJSON_AddStringToObject(argument, "apiBase", api_base);
    if (token != NULL) {
        cJSON_AddStringToObject(argument, "token", token);
    }
    if (previous_json != NULL) {
        cJSON_AddItemToObject(argument, "previous", parsed(previous_json));
    }
    return call_catalog(runtime_marketplace_build, argument);
}

static int count_of(const char *text, const char *fragment) {
    int count = 0;
    for (const char *at = strstr(text, fragment); at != NULL; at = strstr(at + 1, fragment)) {
        count++;
    }
    return count;
}

static int contains(const char *text, const char *fragment) {
    return text != NULL && strstr(text, fragment) != NULL;
}

static void an_id_that_does_not_resolve_is_skipped_and_the_rest_built(void) {
    catalog_answer answer = build("{\"format\":1,\"entries\":[{\"id\":\"github:1\"},{\"id\":\"github:60\"},{\"id\":\"github:2\"}]}");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    const cJSON *entries = cJSON_GetObjectItemCaseSensitive(value_at(answer.tree, "copy"), "entries");
    CHECK(cJSON_GetArraySize(entries) == 2);
    entry_member_is(answer, 0, "id", "github:1");
    entry_member_is(answer, 1, "id", "github:2");
    const cJSON *skipped = cJSON_GetObjectItemCaseSensitive(answer.tree, "skipped");
    CHECK(cJSON_GetArraySize(skipped) == 1);
    check_text(text_at(cJSON_GetArrayItem(skipped, 0), "id"), "github:60", "skipped id");
    CHECK(contains(text_at(cJSON_GetArrayItem(skipped, 0), "reason"), "cannot be looked up"));
    CHECK(contains(text_at(cJSON_GetArrayItem(skipped, 0), "reason"), "not found"));
    cJSON_Delete(answer.tree);
}

static void an_id_that_does_not_resolve_is_carried_over_from_the_previous_copy(void) {
    catalog_answer answer = build_with_options(
        "{\"format\":1,\"entries\":[{\"id\":\"github:1\"},{\"id\":\"github:60\",\"description\":\"New.\"}]}", NULL,
        PREVIOUS_COPY(KEPT_ENTRY));
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(value_at(answer.tree, "copy"), "entries")) == 2);
    entry_member_is(answer, 0, "name", "runtime");
    entry_member_is(answer, 1, "id", "github:60");
    entry_member_is(answer, 1, "name", "kept");
    entry_member_is(answer, 1, "fullName", "spisor/kept");
    entry_member_is(answer, 1, "description", "Old.");
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(answer.tree, "skipped")) == 0);
    cJSON_Delete(answer.tree);
}

static void a_previous_copy_without_the_entry_leaves_it_skipped(void) {
    catalog_answer answer = build_with_options(
        "{\"format\":1,\"entries\":[{\"id\":\"github:60\"}]}", NULL,
        PREVIOUS_COPY("{\"id\":\"github:2\",\"name\":\"weivel\",\"fullName\":\"spisor/weivel\",\"lastKnown\":\"spisor/weivel\"}"));
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(value_at(answer.tree, "copy"), "entries")) == 0);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(answer.tree, "skipped")) == 1);
    cJSON_Delete(answer.tree);
}

static void a_previous_that_is_not_a_copy_is_refused(void) {
    catalog_answer answer = build_with_options("{\"format\":1,\"entries\":[{\"id\":\"github:1\"}]}", NULL, "{\"format\":1}");
    CHECK(expect_refusal(answer, "previous", "a previous that is not a copy"));
    cJSON_Delete(answer.tree);
}

static void a_token_goes_to_each_lookup_and_into_neither_the_copy_nor_a_refusal(void) {
    static const char runtime[] = REPOSITORY("1", "runtime", "spisor/runtime");
    static const char weivel[] = REPOSITORY("2", "weivel", "spisor/weivel");
    const http_route routes[] = {ROUTE("/repositories/1", 200, runtime), ROUTE("/repositories/2", 200, weivel),
                                 EMPTY_ROUTE("/repositories/60", 404)};
    http_fixture *server = http_fixture_start(routes, sizeof routes / sizeof routes[0]);
    CHECK(server != NULL);
    if (server == NULL) {
        return;
    }
    server_port = http_fixture_port(server);
    catalog_answer built = build_with_options(
        "{\"format\":1,\"entries\":[{\"id\":\"github:1\"},{\"id\":\"github:2\"},{\"id\":\"github:60\"}]}", TOKEN, NULL);
    CHECK(built.status == SPISOR_STATUS_SUCCESS);
    char *printed = cJSON_PrintUnformatted(built.tree);
    CHECK(strstr(printed, TOKEN) == NULL);
    cJSON_free(printed);
    cJSON_Delete(built.tree);
    catalog_answer refused =
        build_with_options("{\"format\":1,\"entries\":[{\"id\":\"github:1\"},{\"id\":\"github:1\"}]}", TOKEN, NULL);
    CHECK(expect_refusal(refused, "twice", "a refusal with a token"));
    printed = cJSON_PrintUnformatted(refused.tree);
    CHECK(strstr(printed, TOKEN) == NULL);
    cJSON_free(printed);
    cJSON_Delete(refused.tree);
    char *requests = http_fixture_stop(server);
    CHECK(requests != NULL && count_of(requests, AUTHORIZATION_LINE) == 3 && count_of(requests, "GET /repositories/") == 3);
    free(requests);
}

static void a_token_is_refused_for_a_list_naming_two_hosts_before_any_lookup(void) {
    const http_route routes[] = {EMPTY_ROUTE("/repositories/1", 404)};
    http_fixture *server = http_fixture_start(routes, 1);
    CHECK(server != NULL);
    if (server == NULL) {
        return;
    }
    server_port = http_fixture_port(server);
    const char *lists[] = {"{\"format\":1,\"entries\":[{\"id\":\"github:1\"},{\"id\":\"gitea@codeberg.org:3\"}]}",
                           "{\"format\":1,\"entries\":[{\"id\":\"gitea@codeberg.org:3\"},{\"id\":\"gitea@gitea.example:4\"}]}"};
    for (size_t index = 0; index < 2; index++) {
        catalog_answer answer = build_with_options(lists[index], TOKEN, NULL);
        CHECK(expect_refusal(answer, "more than one host", lists[index]));
        cJSON_Delete(answer.tree);
    }
    char *requests = http_fixture_stop(server);
    CHECK(requests != NULL && requests[0] == 0);
    free(requests);
}

static void run_against_the_server(void) {
    static const char runtime[] = REPOSITORY("1", "runtime", "spisor/runtime");
    static const char weivel[] = REPOSITORY("2", "weivel", "spisor/weivel");
    static const char old_name[] = REPOSITORY("3", "old-name", "spisor/old-name");
    static const char new_name[] = REPOSITORY("3", "new-name", "spisor/new-name");
    static const char another_runtime[] = REPOSITORY("4", "runtime", "other/runtime");
    static const char mixed[] = REPOSITORY("5", "My_Runtime", "spisor/My_Runtime");
    static const char reserved[] = REPOSITORY("6", "Con", "spisor/Con");
    static const char clashing[] = REPOSITORY("8", "My-Runtime", "other/My-Runtime");
    static const char dotted[] = REPOSITORY("9", "__A..B__c", "spisor/__A..B__c");
    static const char unnamed[] = REPOSITORY("10", "___", "spisor/___");
    const http_route routes[] = {
        ROUTE("/repositories/1", 200, runtime),
        ROUTE("/repositories/2", 200, weivel),
        LIMITED_ROUTE("/repositories/3", old_name),
        ROUTE("/repositories/3", 200, new_name),
        ROUTE("/repositories/4", 200, another_runtime),
        ROUTE("/repositories/5", 200, mixed),
        ROUTE("/repositories/6", 200, reserved),
        ROUTE("/repositories/8", 200, clashing),
        ROUTE("/repositories/9", 200, dotted),
        ROUTE("/repositories/10", 200, unnamed),
        EMPTY_ROUTE("/repositories/60", 404),
    };
    http_fixture *server = http_fixture_start(routes, sizeof routes / sizeof routes[0]);
    CHECK(server != NULL);
    if (server == NULL) {
        return;
    }
    server_port = http_fixture_port(server);
    builds_a_copy_with_current_names();
    a_renamed_repo_gets_its_new_name_with_no_change_to_the_list();
    an_alias_replaces_the_name();
    two_entries_with_one_name_and_no_alias_are_refused_naming_both_ids();
    an_alias_settles_a_clash();
    a_clash_is_checked_after_aliases_apply();
    a_duplicate_id_is_refused();
    an_id_that_does_not_resolve_is_skipped_and_the_rest_built();
    an_id_that_does_not_resolve_is_carried_over_from_the_previous_copy();
    a_previous_copy_without_the_entry_leaves_it_skipped();
    a_previous_that_is_not_a_copy_is_refused();
    a_local_id_is_refused();
    a_folder_id_builds_with_the_last_segment_as_its_name();
    two_folders_of_one_repo_build();
    an_alias_replaces_a_folders_name();
    two_folders_with_one_last_segment_clash();
    a_local_folder_id_is_still_refused();
    a_host_name_is_derived_into_an_import_name();
    an_entry_whose_name_cannot_be_derived_is_skipped_and_the_rest_built();
    an_alias_lists_an_entry_whose_name_cannot_be_derived();
    a_clash_after_derivation_refuses_the_whole_build();
    a_list_that_breaks_the_schema_is_refused();
    a_built_time_that_is_not_rfc_3339_is_refused();
    an_empty_list_builds_an_empty_copy();
    free(http_fixture_stop(server));
}

static char *list_of_repositories(size_t count) {
    size_t size = count * 32 + 64;
    char *text = malloc(size);
    size_t used = (size_t)snprintf(text, size, "{\"format\":1,\"entries\":[");
    for (size_t index = 1; index <= count; index++) {
        used += (size_t)snprintf(text + used, size - used, "%s{\"id\":\"github:%zu\"}", index == 1 ? "" : ",", index);
    }
    snprintf(text + used, size - used, "]}");
    return text;
}

static void a_list_over_the_limit_is_refused_before_any_lookup(void) {
    static const char runtime[] = REPOSITORY("1", "runtime", "spisor/runtime");
    const http_route routes[] = {ROUTE("/repositories/1", 200, runtime)};
    http_fixture *server = http_fixture_start(routes, 1);
    CHECK(server != NULL);
    if (server == NULL) {
        return;
    }
    server_port = http_fixture_port(server);
    char *list = list_of_repositories(RUNTIME_MARKETPLACE_MAXIMUM_ENTRIES + 1);
    catalog_answer answer = build(list);
    CHECK(expect_refusal(answer, "list", "a list over the limit"));
    cJSON_Delete(answer.tree);
    free(list);
    char *requests = http_fixture_stop(server);
    CHECK(requests != NULL && requests[0] == 0);
    free(requests);
}

static void a_refusal_that_needs_no_lookup_makes_none(void) {
    static const char runtime[] = REPOSITORY("1", "runtime", "spisor/runtime");
    const http_route routes[] = {ROUTE("/repositories/1", 200, runtime)};
    http_fixture *server = http_fixture_start(routes, 1);
    CHECK(server != NULL);
    if (server == NULL) {
        return;
    }
    server_port = http_fixture_port(server);
    catalog_answer local = build("{\"format\":1,\"entries\":[{\"id\":\"github:1\"},{\"id\":\"local:tool\"}]}");
    CHECK(expect_refusal(local, "local:tool", "a local id after a listed repo"));
    cJSON_Delete(local.tree);
    catalog_answer duplicate = build("{\"format\":1,\"entries\":[{\"id\":\"github:1\"},{\"id\":\"github:1\"}]}");
    CHECK(expect_refusal(duplicate, "twice", "a duplicate"));
    cJSON_Delete(duplicate.tree);
    char *requests = http_fixture_stop(server);
    CHECK(requests != NULL && requests[0] == 0);
    free(requests);
}

static void the_list_checker_follows_the_schema(void) {
    char problem[256];
    cJSON *minimal = parsed("{\"format\":1,\"entries\":[{\"id\":\"github:1\"}]}");
    CHECK(runtime_marketplace_check_list(minimal, problem, sizeof problem));
    cJSON_Delete(minimal);
    cJSON *repeated = parsed("{\"format\":1,\"entries\":[{\"id\":\"github:1\"},{\"id\":\"github:1\"}]}");
    CHECK(runtime_marketplace_check_list(repeated, problem, sizeof problem));
    cJSON_Delete(repeated);
    const char *refused[] = {"{\"format\":1,\"entries\":[{\"alias\":\"core\"}]}",
                             "{\"format\":1,\"entries\":[{\"id\":\"../github:1\"}]}",
                             "{\"format\":1,\"entries\":[{\"id\":\"github:1\",\"name\":\"runtime\"}]}",
                             "{\"format\":1,\"entries\":[{\"id\":\"github:1\",\"categories\":[\"a\",\"a\"]}]}",
                             "{\"format\":1,\"entries\":[{\"id\":\"github:1\",\"categories\":[\"A\"]}]}",
                             "{\"format\":1,\"entries\":[{\"id\":\"github:1\",\"alias\":\"package\"}]}",
                             "{\"format\":1,\"entries\":[],\"extra\":true}", "{\"entries\":[]}"};
    for (size_t index = 0; index < sizeof refused / sizeof refused[0]; index++) {
        cJSON *list = parsed(refused[index]);
        CHECK(!runtime_marketplace_check_list(list, problem, sizeof problem) && problem[0] != 0);
        cJSON_Delete(list);
    }
}

static void the_copy_checker_follows_the_schema(void) {
    char problem[256];
    cJSON *copy = parsed("{\"format\":1,\"built\":\"" BUILT "\",\"entries\":[{\"id\":\"github:1\",\"name\":\"runtime\","
                         "\"fullName\":\"spisor/runtime\",\"lastKnown\":\"spisor/runtime\"}]}");
    CHECK(runtime_marketplace_check_copy(copy, problem, sizeof problem));
    cJSON_Delete(copy);
    const char *refused[] = {
        "{\"format\":1,\"entries\":[]}",
        "{\"format\":1,\"built\":\"2026-13-10T12:00:00Z\",\"entries\":[]}",
        "{\"format\":1,\"built\":\"" BUILT "\",\"entries\":[{\"id\":\"github:1\",\"name\":\"runtime\",\"fullName\":\"spisor/runtime\"}]}",
        "{\"format\":1,\"built\":\"" BUILT "\",\"entries\":[{\"id\":\"github:1\",\"name\":\"runtime\",\"fullName\":\"runtime\",\"lastKnown\":\"a/b\"}]}",
        "{\"format\":1,\"built\":\"" BUILT "\",\"entries\":[{\"id\":\"github:1\",\"name\":\"Runtime\",\"fullName\":\"a/b\",\"lastKnown\":\"a/b\"}]}"};
    for (size_t index = 0; index < sizeof refused / sizeof refused[0]; index++) {
        cJSON *bad = parsed(refused[index]);
        CHECK(!runtime_marketplace_check_copy(bad, problem, sizeof problem) && problem[0] != 0);
        cJSON_Delete(bad);
    }
}

static void the_time_and_full_name_checks_follow_their_patterns(void) {
    CHECK(runtime_marketplace_is_built_time("2026-11-10T12:00:00Z"));
    CHECK(runtime_marketplace_is_built_time("2026-11-10t12:00:00.123+01:00"));
    CHECK(runtime_marketplace_is_built_time("2016-12-31T23:59:60z"));
    CHECK(!runtime_marketplace_is_built_time("2026-11-10T12:00:00"));
    CHECK(!runtime_marketplace_is_built_time("2026-11-10 12:00:00Z"));
    CHECK(!runtime_marketplace_is_built_time("2026-11-10T24:00:00Z"));
    CHECK(!runtime_marketplace_is_built_time("2026-11-10T12:00:00Z "));
    CHECK(runtime_marketplace_is_full_name("spisor/runtime"));
    CHECK(runtime_marketplace_is_full_name("Org-1/Name_2.x"));
    CHECK(!runtime_marketplace_is_full_name("runtime"));
    CHECK(!runtime_marketplace_is_full_name("a/b/c"));
    CHECK(!runtime_marketplace_is_full_name("a/.."));
    CHECK(!runtime_marketplace_is_full_name("-a/b"));
    CHECK(!runtime_marketplace_is_full_name("a/"));
}

static int held_value_matches(const cJSON *schema, const runtime_marketplace_held_value *held) {
    const cJSON *node = value_at(schema, held->path);
    if (cJSON_IsString(node)) {
        return strcmp(node->valuestring, held->value) == 0;
    }
    char *printed = node == NULL ? NULL : cJSON_PrintUnformatted(node);
    int matched = printed != NULL && strcmp(printed, held->value) == 0;
    cJSON_free(printed);
    return matched;
}

static void the_checkers_hold_what_the_schemas_say(void) {
    cJSON *list_schema = read_json_file(RUNTIME_MARKETPLACE_SCHEMA_PATH);
    cJSON *copy_schema = read_json_file(RUNTIME_MARKETPLACE_COPY_SCHEMA_PATH);
    for (size_t index = 0; index < runtime_marketplace_held_value_count; index++) {
        const runtime_marketplace_held_value *held = &runtime_marketplace_held_values[index];
        int is_list = strcmp(held->schema_file, "marketplace.schema.json") == 0;
        if (!held_value_matches(is_list ? list_schema : copy_schema, held)) {
            fprintf(stderr, "%s no longer says %s at %s; change marketplace_check.c with it\n", held->schema_file,
                    held->value, held->path);
            check_failures++;
        }
    }
    CHECK(runtime_marketplace_held_value_count > 0);
    cJSON_Delete(list_schema);
    cJSON_Delete(copy_schema);
}

#if !defined(_WIN32)
static void clear_proxy_environment(void) {
    const char *names[] = {"http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY", "all_proxy",
                           "ALL_PROXY",  "no_proxy",   "NO_PROXY"};
    for (size_t index = 0; index < sizeof names / sizeof names[0]; index++) {
        unsetenv(names[index]);
    }
}
#endif

int main(void) {
#if !defined(_WIN32)
    clear_proxy_environment();
#endif
    runtime_fetch_allow_plain_http(1);
    run_against_the_server();
    a_list_over_the_limit_is_refused_before_any_lookup();
    a_refusal_that_needs_no_lookup_makes_none();
    a_token_goes_to_each_lookup_and_into_neither_the_copy_nor_a_refusal();
    a_token_is_refused_for_a_list_naming_two_hosts_before_any_lookup();
    the_list_checker_follows_the_schema();
    the_copy_checker_follows_the_schema();
    the_time_and_full_name_checks_follow_their_patterns();
    the_checkers_hold_what_the_schemas_say();
    return check_summary("marketplace_build_test");
}
