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
#include "marketplace_name_lookup.h"
#include "marketplace_read.h"
#include "marketplace_sources.h"
#include "scratch_directory.h"
#include "spisor/file.h"

#if defined(_WIN32)
#include <process.h>
#define current_process_id() _getpid()
#else
#include <unistd.h>
#define current_process_id() getpid()
#endif

#define BUILT "2026-11-10T12:00:00Z"
#define COPY_ONE                                                                                                    \
    "{\"format\":1,\"title\":\"One\",\"built\":\"" BUILT "\",\"entries\":["                                       \
    "{\"id\":\"github:1\",\"name\":\"runtime\",\"fullName\":\"spisor/runtime\",\"lastKnown\":\"spisor/runtime\"},"   \
    "{\"id\":\"github:2\",\"name\":\"weivel\",\"fullName\":\"spisor/weivel\",\"lastKnown\":\"spisor/weivel\"}]}"
#define COPY_TWO                                                                                                    \
    "{\"format\":1,\"built\":\"" BUILT "\",\"entries\":["                                                          \
    "{\"id\":\"github:9\",\"name\":\"runtime\",\"fullName\":\"other/runtime\",\"lastKnown\":\"other/runtime\"},"     \
    "{\"id\":\"github:10\",\"name\":\"extra\",\"fullName\":\"other/extra\",\"lastKnown\":\"other/extra\"}]}"
#define COPY_MIRROR                                                                                                     "{\"format\":1,\"built\":\"" BUILT "\",\"entries\":["                                                              "{\"id\":\"github:1\",\"name\":\"runtime\",\"fullName\":\"mirror/runtime\",\"lastKnown\":\"mirror/runtime\"}]}"
#define COPY_SAME_NAME                                                                                              \
    "{\"format\":1,\"built\":\"" BUILT "\",\"entries\":["                                                          \
    "{\"id\":\"github:1\",\"name\":\"same\",\"fullName\":\"a/same\",\"lastKnown\":\"a/same\"},"                     \
    "{\"id\":\"github:2\",\"name\":\"same\",\"fullName\":\"b/same\",\"lastKnown\":\"b/same\"}]}"
#define LIST_TEXT "{\"format\":1,\"entries\":[{\"id\":\"github:1\"}]}"
#define HTML_TEXT "<!doctype html><html><body>not a copy</body></html>"
#define ROUTE(path, status, body) {path, status, NULL, body, sizeof body - 1, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0}

static unsigned short server_port;
static char scratch[1024];

static cJSON *source_object(const char *id, const char *url) {
    cJSON *source = cJSON_CreateObject();
    cJSON_AddStringToObject(source, "id", id);
    cJSON_AddStringToObject(source, "url", url);
    return source;
}

static void url_on_server(const char *path, char *url, size_t capacity) {
    snprintf(url, capacity, "http://127.0.0.1:%u%s", (unsigned)server_port, path);
}

static catalog_answer sources_of(const char *marketplaces_json) {
    cJSON *settings = cJSON_CreateObject();
    cJSON_AddItemToObject(settings, "marketplaces", parsed(marketplaces_json));
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "settings", settings);
    return call_catalog(runtime_marketplace_sources, argument);
}

static catalog_answer read_text(const char *id, const char *text) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "source", source_object(id, "https://example.com/marketplace.json"));
    cJSON_AddStringToObject(argument, "text", text);
    return call_catalog(runtime_marketplace_read, argument);
}

static catalog_answer read_fetched(const char *id, const char *url) {
    char destination[1200];
    snprintf(destination, sizeof destination, "%s/download.json", scratch);
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "source", source_object(id, url));
    cJSON_AddStringToObject(argument, "destination", destination);
    return call_catalog(runtime_marketplace_read, argument);
}

static const cJSON *entry_of(const catalog_answer answer, int index) {
    return cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(value_at(answer.tree, "copy"), "entries"), index);
}

static cJSON *copy_pair(const char *id, const char *copy_json) {
    cJSON *pair = cJSON_CreateObject();
    cJSON_AddItemToObject(pair, "source", source_object(id, "https://example.com/marketplace.json"));
    cJSON_AddItemToObject(pair, "copy", parsed(copy_json));
    return pair;
}

static catalog_answer lookup_in(const char *name) {
    cJSON *argument = cJSON_CreateObject();
    cJSON *copies = cJSON_AddArrayToObject(argument, "copies");
    cJSON_AddItemToArray(copies, copy_pair("spisor", COPY_ONE));
    cJSON_AddItemToArray(copies, copy_pair("other", COPY_TWO));
    cJSON_AddStringToObject(argument, "name", name);
    return call_catalog(runtime_marketplace_lookup, argument);
}

static catalog_answer lookup_three(const char *middle_copy, const char *name) {
    cJSON *argument = cJSON_CreateObject();
    cJSON *copies = cJSON_AddArrayToObject(argument, "copies");
    cJSON_AddItemToArray(copies, copy_pair("spisor", COPY_ONE));
    cJSON_AddItemToArray(copies, copy_pair("middle", middle_copy));
    cJSON_AddItemToArray(copies, copy_pair("other", COPY_TWO));
    cJSON_AddStringToObject(argument, "name", name);
    return call_catalog(runtime_marketplace_lookup, argument);
}

static int is_not_found(const catalog_answer answer) {
    return answer.status == SPISOR_STATUS_SUCCESS && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answer.tree, "notFound"));
}

static int message_names(const catalog_answer answer, const char *fragment) {
    const char *message = text_at(answer.tree, "message");
    return message != NULL && strstr(message, fragment) != NULL;
}

static void sources_answers_each_source(void) {
    catalog_answer answer = sources_of("[{\"id\":\"spisor\",\"url\":\"https://example.com/m.json\"},"
                                       "{\"id\":\"local\",\"url\":\"file:///C:/m/copy.json\"},"
                                       "{\"id\":\"private\",\"provider\":\"corp-marketplace\"}]");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    const cJSON *sources = value_at(answer.tree, "sources");
    CHECK(cJSON_GetArraySize(sources) == 3);
    check_text(text_at(cJSON_GetArrayItem(sources, 0), "id"), "spisor", "first id");
    check_text(text_at(cJSON_GetArrayItem(sources, 0), "url"), "https://example.com/m.json", "first url");
    check_text(text_at(cJSON_GetArrayItem(sources, 1), "url"), "file:///C:/m/copy.json", "file url");
    check_text(text_at(cJSON_GetArrayItem(sources, 2), "provider"), "corp-marketplace", "provider");
    CHECK(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(sources, 2), "url") == NULL);
    CHECK(cJSON_GetArraySize(value_at(answer.tree, "skipped")) == 0);
    cJSON_Delete(answer.tree);
}

static void sources_without_the_setting_answers_none(void) {
    catalog_answer answer = call_catalog(runtime_marketplace_sources, parsed("{\"settings\":{}}"));
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    CHECK(cJSON_GetArraySize(value_at(answer.tree, "sources")) == 0);
    cJSON_Delete(answer.tree);
}

static void a_source_id_that_is_not_an_import_name_is_skipped_and_reported(void) {
    catalog_answer answer = sources_of("[{\"id\":\"Not A Name\",\"url\":\"https://example.com/a.json\"},"
                                       "{\"id\":\"package\",\"url\":\"https://example.com/p.json\"},"
                                       "{\"id\":\"fine\",\"url\":\"https://example.com/f.json\"}]");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    const cJSON *sources = value_at(answer.tree, "sources");
    CHECK(cJSON_GetArraySize(sources) == 1);
    check_text(text_at(cJSON_GetArrayItem(sources, 0), "id"), "fine", "kept id");
    const cJSON *skipped = value_at(answer.tree, "skipped");
    CHECK(cJSON_GetArraySize(skipped) == 2);
    CHECK(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(skipped, 0), "index")->valueint == 0);
    CHECK(strstr(text_at(cJSON_GetArrayItem(skipped, 0), "detail"), "Not A Name") != NULL);
    CHECK(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(skipped, 1), "index")->valueint == 1);
    cJSON_Delete(answer.tree);
}

static void a_source_without_one_usable_location_is_skipped(void) {
    catalog_answer answer = sources_of("[{\"id\":\"ftp\",\"url\":\"ftp://example.com/m.json\"},"
                                       "{\"id\":\"plain\",\"url\":\"http://example.com/m.json\"},"
                                       "{\"id\":\"neither\"},"
                                       "{\"id\":\"both\",\"url\":\"https://example.com/m.json\",\"provider\":\"corp\"},"
                                       "7,"
                                       "{\"id\":\"ok\",\"url\":\"https://example.com/m.json\"}]");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    CHECK(cJSON_GetArraySize(value_at(answer.tree, "sources")) == 1);
    CHECK(cJSON_GetArraySize(value_at(answer.tree, "skipped")) == 5);
    cJSON_Delete(answer.tree);
}

static void two_sources_with_one_id_are_refused_naming_both(void) {
    catalog_answer answer = sources_of("[{\"id\":\"spisor\",\"url\":\"https://first.example.com/m.json\"},"
                                       "{\"id\":\"other\",\"url\":\"https://example.com/o.json\"},"
                                       "{\"id\":\"spisor\",\"url\":\"https://second.example.com/m.json\"}]");
    expect_refusal(answer, "first.example.com", "duplicate id names the first");
    CHECK(message_names(answer, "second.example.com"));
    CHECK(message_names(answer, "spisor"));
    cJSON_Delete(answer.tree);
}

static void sources_refuses_a_call_without_settings(void) {
    catalog_answer answer = call_catalog(runtime_marketplace_sources, parsed("{}"));
    expect_refusal(answer, "settings", "no settings");
    cJSON_Delete(answer.tree);
}

static void reads_a_copy(void) {
    catalog_answer answer = read_text("spisor", COPY_ONE);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(value_at(answer.tree, "copy"), "entries")) == 2);
    check_text(text_at(entry_of(answer, 1), "name"), "weivel", "second name");
    cJSON_Delete(answer.tree);
}

static void reads_a_copy_with_a_byte_order_mark(void) {
    catalog_answer answer = read_text("spisor", "\xEF\xBB\xBF" COPY_ONE);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(answer.tree);
}

static void reads_a_copy_fetched_over_the_network(void) {
    char url[256];
    url_on_server("/copy.json", url, sizeof url);
    catalog_answer answer = read_fetched("spisor", url);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    check_text(text_at(entry_of(answer, 0), "name"), "runtime", "fetched name");
    cJSON_Delete(answer.tree);
}

static void reads_a_copy_from_a_file_url(void) {
    char path[1200];
    char url[1300];
    snprintf(path, sizeof path, "%s/copy.json", scratch);
    CHECK(spisor_replace_file(path, (const uint8_t *)COPY_ONE, sizeof COPY_ONE - 1) == 1);
    snprintf(url, sizeof url, path[0] == '/' ? "file://%s" : "file:///%s", path);
    catalog_answer answer = read_fetched("spisor", url);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(answer.tree);
}

static void refuses_a_list_where_a_copy_is_expected_with_a_fix_to_build_it(void) {
    catalog_answer answer = read_text("spisor", LIST_TEXT);
    expect_refusal(answer, "spisor", "list names the source");
    CHECK(message_names(answer, "list"));
    CHECK(strstr(text_at(answer.tree, "fix"), "marketplace build") != NULL);
    cJSON_Delete(answer.tree);
}

static void refuses_html_naming_the_source(void) {
    catalog_answer inline_text = read_text("spisor", HTML_TEXT);
    expect_refusal(inline_text, "spisor", "html text names the source");
    CHECK(message_names(inline_text, "web page"));
    cJSON_Delete(inline_text.tree);
    char url[256];
    url_on_server("/page.html", url, sizeof url);
    catalog_answer fetched = read_fetched("spisor", url);
    expect_refusal(fetched, "spisor", "html fetched names the source");
    CHECK(message_names(fetched, "web page"));
    cJSON_Delete(fetched.tree);
}

static char *spaces_over_the_limit(void) {
    size_t length = RUNTIME_MARKETPLACE_MAXIMUM_BYTES + 1;
    char *body = malloc(length + 1);
    if (body != NULL) {
        memset(body, ' ', length);
        body[length] = '\0';
    }
    return body;
}

static void refuses_a_body_over_the_limit_naming_the_source(void) {
    char url[256];
    url_on_server("/huge.json", url, sizeof url);
    catalog_answer fetched = read_fetched("spisor", url);
    expect_refusal(fetched, "spisor", "huge fetched names the source");
    CHECK(message_names(fetched, "maxBytes"));
    cJSON_Delete(fetched.tree);
    char *huge = spaces_over_the_limit();
    CHECK(huge != NULL);
    if (huge == NULL) {
        return;
    }
    catalog_answer inline_text = read_text("spisor", huge);
    expect_refusal(inline_text, "spisor", "huge text names the source");
    CHECK(message_names(inline_text, "limit"));
    cJSON_Delete(inline_text.tree);
    free(huge);
}

static void refuses_text_that_is_not_json_and_a_broken_copy(void) {
    catalog_answer not_json = read_text("spisor", "name: yaml");
    expect_refusal(not_json, "spisor", "not json");
    cJSON_Delete(not_json.tree);
    catalog_answer broken =
        read_text("spisor", "{\"format\":1,\"built\":\"" BUILT "\",\"entries\":[{\"id\":\"github:1\"}]}");
    expect_refusal(broken, "spisor", "broken copy");
    cJSON_Delete(broken.tree);
    catalog_answer array = read_text("spisor", "[]");
    expect_refusal(array, "spisor", "array");
    cJSON_Delete(array.tree);
}

static void refuses_a_copy_naming_two_entries_alike(void) {
    catalog_answer answer = read_text("spisor", COPY_SAME_NAME);
    expect_refusal(answer, "github:1", "same name names both ids");
    CHECK(message_names(answer, "github:2"));
    cJSON_Delete(answer.tree);
}

static void refuses_a_provider_source_and_an_unusable_call(void) {
    catalog_answer provider = call_catalog(
        runtime_marketplace_read, parsed("{\"source\":{\"id\":\"private\",\"provider\":\"corp\"},\"text\":\"{}\"}"));
    expect_refusal(provider, "private", "provider source");
    cJSON_Delete(provider.tree);
    catalog_answer bad_id = call_catalog(
        runtime_marketplace_read,
        parsed("{\"source\":{\"id\":\"Bad Id\",\"url\":\"https://e.com/m.json\"},\"text\":\"{}\"}"));
    expect_refusal(bad_id, "source", "bad id");
    cJSON_Delete(bad_id.tree);
    catalog_answer neither = call_catalog(
        runtime_marketplace_read, parsed("{\"source\":{\"id\":\"ok\",\"url\":\"https://e.com/m.json\"}}"));
    expect_refusal(neither, "text", "neither text nor destination");
    cJSON_Delete(neither.tree);
}

static void one_bad_source_does_not_stop_the_others(void) {
    char html_url[256];
    char good_url[256];
    char huge_url[256];
    url_on_server("/page.html", html_url, sizeof html_url);
    url_on_server("/copy.json", good_url, sizeof good_url);
    url_on_server("/huge.json", huge_url, sizeof huge_url);
    catalog_answer first = read_fetched("html", html_url);
    catalog_answer second = read_fetched("good", good_url);
    catalog_answer third = read_fetched("huge", huge_url);
    catalog_answer fourth = read_fetched("good", good_url);
    CHECK(first.status == SPISOR_STATUS_FAILURE);
    CHECK(second.status == SPISOR_STATUS_SUCCESS);
    CHECK(third.status == SPISOR_STATUS_FAILURE);
    CHECK(fourth.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(first.tree);
    cJSON_Delete(second.tree);
    cJSON_Delete(third.tree);
    cJSON_Delete(fourth.tree);
}

static void a_bare_name_in_one_marketplace_settles(void) {
    catalog_answer answer = lookup_in("weivel");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    check_text(text_at(answer.tree, "id"), "github:2", "id");
    check_text(text_at(answer.tree, "marketplace"), "spisor", "marketplace");
    cJSON_Delete(answer.tree);
}

static void a_bare_name_in_two_marketplaces_is_ambiguous(void) {
    catalog_answer answer = lookup_in("runtime");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    const cJSON *ambiguous = value_at(answer.tree, "ambiguous");
    CHECK(cJSON_GetArraySize(ambiguous) == 2);
    check_text(cJSON_GetArrayItem(ambiguous, 0)->valuestring, "spisor/runtime", "first form");
    check_text(cJSON_GetArrayItem(ambiguous, 1)->valuestring, "other/runtime", "second form");
    CHECK(cJSON_GetObjectItemCaseSensitive(answer.tree, "id") == NULL);
    cJSON_Delete(answer.tree);
}

static void the_same_id_in_two_marketplaces_settles_on_the_first(void) {
    cJSON *argument = cJSON_CreateObject();
    cJSON *copies = cJSON_AddArrayToObject(argument, "copies");
    cJSON_AddItemToArray(copies, copy_pair("spisor", COPY_ONE));
    cJSON_AddItemToArray(copies, copy_pair("mirror", COPY_MIRROR));
    cJSON_AddStringToObject(argument, "name", "runtime");
    catalog_answer answer = call_catalog(runtime_marketplace_lookup, argument);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    check_text(text_at(answer.tree, "id"), "github:1", "id");
    check_text(text_at(answer.tree, "marketplace"), "spisor", "first marketplace in source order");
    CHECK(cJSON_GetObjectItemCaseSensitive(answer.tree, "ambiguous") == NULL);
    cJSON_Delete(answer.tree);
}

static void two_copies_sharing_an_id_and_a_third_with_another_are_ambiguous(void) {
    catalog_answer answer = lookup_three(COPY_MIRROR, "runtime");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    const cJSON *ambiguous = value_at(answer.tree, "ambiguous");
    CHECK(cJSON_GetArraySize(ambiguous) == 3);
    check_text(cJSON_GetArrayItem(ambiguous, 0)->valuestring, "spisor/runtime", "first form");
    check_text(cJSON_GetArrayItem(ambiguous, 1)->valuestring, "middle/runtime", "second form");
    check_text(cJSON_GetArrayItem(ambiguous, 2)->valuestring, "other/runtime", "third form");
    cJSON_Delete(answer.tree);
}

static void a_qualified_name_picks_its_marketplace(void) {
    catalog_answer first = lookup_in("spisor/runtime");
    check_text(text_at(first.tree, "id"), "github:1", "spisor id");
    check_text(text_at(first.tree, "marketplace"), "spisor", "spisor marketplace");
    cJSON_Delete(first.tree);
    catalog_answer second = lookup_in("other/runtime");
    check_text(text_at(second.tree, "id"), "github:9", "other id");
    check_text(text_at(second.tree, "marketplace"), "other", "other marketplace");
    cJSON_Delete(second.tree);
}

static void a_qualified_name_searches_only_its_marketplace(void) {
    catalog_answer answer = lookup_in("spisor/extra");
    CHECK(is_not_found(answer));
    cJSON_Delete(answer.tree);
}

static void a_marketplace_with_no_copy_answers_not_found(void) {
    catalog_answer answer = lookup_in("missing/runtime");
    CHECK(is_not_found(answer));
    cJSON_Delete(answer.tree);
}

static void an_unknown_name_answers_not_found_and_matching_is_exact(void) {
    const char *names[] = {"nothing", "Runtime", "runtim", "runtime/", "spisor/runtime/x"};
    for (size_t index = 0; index < sizeof names / sizeof names[0]; index++) {
        catalog_answer answer = lookup_in(names[index]);
        CHECK(is_not_found(answer));
        cJSON_Delete(answer.tree);
    }
}

static void lookup_refuses_an_unusable_call(void) {
    catalog_answer empty_name = lookup_in("");
    expect_refusal(empty_name, "name", "empty name");
    cJSON_Delete(empty_name.tree);
    catalog_answer list = call_catalog(
        runtime_marketplace_lookup,
        parsed("{\"name\":\"x\",\"copies\":[{\"source\":{\"id\":\"spisor\"},\"copy\":" LIST_TEXT "}]}"));
    expect_refusal(list, "spisor", "a list in place of a copy");
    cJSON_Delete(list.tree);
    cJSON *twice = parsed("{\"name\":\"x\",\"copies\":[]}");
    cJSON *copies = cJSON_GetObjectItemCaseSensitive(twice, "copies");
    cJSON_AddItemToArray(copies, copy_pair("spisor", COPY_ONE));
    cJSON_AddItemToArray(copies, copy_pair("spisor", COPY_TWO));
    catalog_answer duplicate = call_catalog(runtime_marketplace_lookup, twice);
    expect_refusal(duplicate, "spisor", "two copies of one source");
    cJSON_Delete(duplicate.tree);
}

static void a_destination_that_exists_is_refused_and_left_alone(void) {
    char path[1200];
    snprintf(path, sizeof path, "%s/existing.json", scratch);
    CHECK(spisor_replace_file(path, (const uint8_t *)"keep", 4) == 1);
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "source", source_object("spisor", "https://example.com/m.json"));
    cJSON_AddStringToObject(argument, "destination", path);
    catalog_answer answer = call_catalog(runtime_marketplace_read, argument);
    expect_refusal(answer, "already exists", "existing destination");
    cJSON_Delete(answer.tree);
    CHECK(spisor_file_exists(path));
}

static void a_url_that_is_not_file_or_https_is_refused_by_read(void) {
    char destination[1200];
    snprintf(destination, sizeof destination, "%s/never.json", scratch);
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "source", source_object("spisor", "ftp://example.com/m.json"));
    cJSON_AddStringToObject(argument, "destination", destination);
    catalog_answer answer = call_catalog(runtime_marketplace_read, argument);
    expect_refusal(answer, "file: or https:", "scheme");
    CHECK(message_names(answer, "spisor"));
    cJSON_Delete(answer.tree);
}

static void run_against_the_server(void) {
    static const char copy_one[] = COPY_ONE;
    static const char html[] = HTML_TEXT;
    char *huge = spaces_over_the_limit();
    CHECK(huge != NULL);
    if (huge == NULL) {
        return;
    }
    const http_route routes[] = {
        ROUTE("/copy.json", 200, copy_one),
        ROUTE("/page.html", 200, html),
        {"/huge.json", 200, NULL, huge, RUNTIME_MARKETPLACE_MAXIMUM_BYTES + 1, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0},
    };
    http_fixture *server = http_fixture_start(routes, sizeof routes / sizeof routes[0]);
    CHECK(server != NULL);
    if (server != NULL) {
        server_port = http_fixture_port(server);
        reads_a_copy_fetched_over_the_network();
        refuses_html_naming_the_source();
        refuses_a_body_over_the_limit_naming_the_source();
        one_bad_source_does_not_stop_the_others();
        free(http_fixture_stop(server));
    }
    free(huge);
}

static void run_without_a_server(void) {
    sources_answers_each_source();
    sources_without_the_setting_answers_none();
    a_source_id_that_is_not_an_import_name_is_skipped_and_reported();
    a_source_without_one_usable_location_is_skipped();
    two_sources_with_one_id_are_refused_naming_both();
    sources_refuses_a_call_without_settings();
    reads_a_copy();
    reads_a_copy_with_a_byte_order_mark();
    reads_a_copy_from_a_file_url();
    refuses_a_list_where_a_copy_is_expected_with_a_fix_to_build_it();
    refuses_text_that_is_not_json_and_a_broken_copy();
    refuses_a_copy_naming_two_entries_alike();
    refuses_a_provider_source_and_an_unusable_call();
    a_bare_name_in_one_marketplace_settles();
    a_bare_name_in_two_marketplaces_is_ambiguous();
    the_same_id_in_two_marketplaces_settles_on_the_first();
    two_copies_sharing_an_id_and_a_third_with_another_are_ambiguous();
    a_destination_that_exists_is_refused_and_left_alone();
    a_url_that_is_not_file_or_https_is_refused_by_read();
    a_qualified_name_picks_its_marketplace();
    a_qualified_name_searches_only_its_marketplace();
    a_marketplace_with_no_copy_answers_not_found();
    an_unknown_name_answers_not_found_and_matching_is_exact();
    lookup_refuses_an_unusable_call();
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
    snprintf(scratch, sizeof scratch, "%s/marketplace-sources-%d", RUNTIME_SCRATCH_PATH, (int)current_process_id());
    start_empty_directory(scratch);
    runtime_fetch_allow_plain_http(1);
    run_without_a_server();
    run_against_the_server();
    CHECK(runtime_remove_tree(scratch) == 1);
    return check_summary("marketplace_sources_test");
}
