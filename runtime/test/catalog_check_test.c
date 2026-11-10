#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog.h"
#include "catalog_call.h"
#include "catalog_hooks.h"
#include "catalog_state.h"
#include "check.h"
#include "fetch_hooks.h"
#include "http_fixture.h"
#include "scratch_directory.h"
#include "sha256.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"

#if defined(_WIN32)
#include <process.h>
#define current_process_id() _getpid()
#else
#include <unistd.h>
#define current_process_id() getpid()
#endif

#if defined(_WIN32)
#define RUNTIME_SEPARATOR '\\'
#else
#define RUNTIME_SEPARATOR '/'
#endif

#define URL "https://catalog.test/index.json"
#define START "2026-11-03T08:00:00.000Z"
#define LATER_1 "2026-11-03T09:00:00.000Z"
#define LATER_2 "2026-11-03T10:00:00.000Z"
#define LATER_23 "2026-11-04T07:00:00.000Z"
#define LATER_25 "2026-11-04T09:00:00.000Z"
#define LATER_26 "2026-11-04T10:00:00.000Z"
#define ETAG "\"one\""
#define ZEROES "0000000000000000000000000000000000000000000000000000000000000000"

static char base[1024];
static char cache[1100];

static char *index_text(const char *latest) {
    char *text = malloc(512);
    snprintf(text, 512,
             "{\"format\":1,\"items\":{\"a\":{\"kind\":\"plugin\",\"latest\":\"%s\",\"versions\":{\"%s\":{\"files\":{"
             "\"plugin.json\":{\"url\":\"a/%s/plugin.json\",\"sha256\":\"" ZEROES "\",\"size\":1}}}}}}}",
             latest, latest, latest);
    return text;
}

static void start_scratch(const char *label) {
    if (base[0] != 0) {
        CHECK(runtime_remove_tree(base) == 1);
    }
    snprintf(base, sizeof base, "%s/catalog-check-J\xc3\xb6rg-%d-%s", RUNTIME_SCRATCH_PATH, (int)current_process_id(),
             label);
    snprintf(cache, sizeof cache, "%s/cache", base);
    start_empty_directory(base);
}

typedef struct served {
    http_fixture *fixture;
    char *body;
    http_route route;
} served;

/** Serves body at /index.json, carrying etag when it is not NULL, and sends https://catalog.test there. */
static void serve(served *server, int status, const char *body, const char *etag) {
    server->body = body == NULL ? NULL : malloc(strlen(body) + 1);
    if (body != NULL) {
        strcpy(server->body, body);
    }
    http_route route = {"/index.json", status, NULL, server->body == NULL ? "" : server->body,
                        server->body == NULL ? 0 : strlen(server->body), HTTP_ROUTE_BODY_LENGTH, etag, NULL, 0, 0};
    server->route = route;
    server->fixture = http_fixture_start(&server->route, 1);
    static char target[64];
    snprintf(target, sizeof target, "http://127.0.0.1:%u", (unsigned)http_fixture_port(server->fixture));
    runtime_catalog_redirect_requests("https://catalog.test", target);
}

static size_t count_of(const char *text, const char *fragment) {
    size_t count = 0;
    for (const char *at = text == NULL ? NULL : strstr(text, fragment); at != NULL; at = strstr(at + 1, fragment)) {
        count++;
    }
    return count;
}

/** Stops the server, answering how many requests it saw; heads, when not NULL, takes the request heads. */
static size_t stop(served *server, char **heads) {
    char *seen = http_fixture_stop(server->fixture);
    size_t requests = count_of(seen, "GET ");
    free(server->body);
    if (heads != NULL) {
        *heads = seen;
    } else {
        free(seen);
    }
    runtime_catalog_redirect_requests(NULL, NULL);
    return requests;
}

/** Points https://catalog.test at a port nothing listens on, as a network that is down. */
static void serve_nothing(void) {
    static char target[64];
    http_fixture *fixture = http_fixture_start(NULL, 0);
    snprintf(target, sizeof target, "http://127.0.0.1:%u", (unsigned)http_fixture_port(fixture));
    free(http_fixture_stop(fixture));
    runtime_catalog_redirect_requests("https://catalog.test", target);
}

static cJSON *source_json(const char *kind, const char *location) {
    cJSON *source = cJSON_CreateObject();
    cJSON_AddStringToObject(source, "kind", kind);
    cJSON_AddStringToObject(source, strcmp(kind, "local") == 0 ? "path" : "url", location);
    return source;
}

static catalog_answer check_source(const char *kind, const char *location, const char *now) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "source", source_json(kind, location));
    cJSON_AddStringToObject(argument, "cache", cache);
    cJSON_AddStringToObject(argument, "now", now);
    cJSON_AddNumberToObject(argument, "intervalHours", 24);
    return call_catalog(runtime_catalog_check, argument);
}

static const char *kind_of(catalog_answer answer) {
    return text_at(answer.tree, "kind");
}

static void expect_kind(catalog_answer answer, const char *kind, const char *label) {
    check_text(kind_of(answer), kind, label);
    if (strcmp(kind, "failed") != 0 && kind_of(answer) != NULL && strcmp(kind_of(answer), "failed") == 0) {
        fprintf(stderr, "%s: failed with %s\n", label, text_at(answer.tree, "detail"));
    }
    cJSON_Delete(answer.tree);
}

static char *state_file(const char *location) {
    return runtime_catalog_state_path(cache, location);
}

static cJSON *state_of(const char *location) {
    char *path = state_file(location);
    cJSON *state = read_json_file(path);
    free(path);
    return state;
}

static cJSON *current_of(cJSON *sources) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "sources", sources);
    cJSON_AddStringToObject(argument, "cache", cache);
    catalog_answer answer = call_catalog(runtime_catalog_current, argument);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    cJSON *catalog = cJSON_DetachItemFromObjectCaseSensitive(answer.tree, "catalog");
    cJSON_Delete(answer.tree);
    return catalog;
}

static cJSON *current_of_url(void) {
    cJSON *sources = cJSON_CreateArray();
    cJSON_AddItemToArray(sources, source_json("https", URL));
    return current_of(sources);
}

static void expect_current_latest(const char *latest, const char *label) {
    cJSON *catalog = current_of_url();
    check_text(text_at(value_at(catalog, "items/a"), "latest"), latest, label);
    cJSON_Delete(catalog);
}

static void check_once(int status, const char *latest, const char *etag, const char *now, const char *expected_kind) {
    served server;
    char *body = latest == NULL ? NULL : index_text(latest);
    serve(&server, status, body, etag);
    expect_kind(check_source("https", URL, now), expected_kind, now);
    stop(&server, NULL);
    free(body);
}

static void it_makes_one_request_for_two_checks_inside_the_interval(void) {
    start_scratch("one-request");
    served server;
    char *body = index_text("1.0.0");
    serve(&server, 200, body, ETAG);
    expect_kind(check_source("https", URL, START), "changed", "first check");
    expect_kind(check_source("https", URL, LATER_23), "skipped", "inside the interval");
    CHECK(stop(&server, NULL) == 1);
    free(body);
}

static void it_keeps_the_index_its_etag_and_the_time_under_the_catalog_folder(void) {
    start_scratch("keeps");
    check_once(200, "1.0.0", ETAG, START, "changed");
    char *path = state_file(URL);
    char hex[RUNTIME_SHA256_HEX_SIZE];
    runtime_sha256 hash;
    runtime_sha256_start(&hash);
    runtime_sha256_add(&hash, (const uint8_t *)URL, strlen(URL));
    runtime_sha256_finish_hex(&hash, hex);
    char expected_name[128];
    snprintf(expected_name, sizeof expected_name, "catalog%c%s.json", RUNTIME_SEPARATOR, hex);
    size_t length = strlen(path);
    CHECK(length > strlen(expected_name) && strcmp(path + length - strlen(expected_name), expected_name) == 0);
    CHECK(spisor_file_exists(path));
    free(path);
    cJSON *state = state_of(URL);
    char *body = index_text("1.0.0");
    char expected[1024];
    cJSON *wanted = cJSON_CreateObject();
    cJSON_AddStringToObject(wanted, "etag", ETAG);
    cJSON_AddStringToObject(wanted, "checkedAt", START);
    cJSON_AddStringToObject(wanted, "index", body);
    CHECK(cJSON_Compare(state, wanted, 1));
    snprintf(expected, sizeof expected, "%s", "https://catalog.test/a/1.0.0/plugin.json");
    cJSON *catalog = current_of_url();
    const cJSON *url = value_at(catalog, "items/a/versions/1.0.0/files/plugin.json/url");
    check_text(cJSON_IsString(url) ? url->valuestring : NULL, expected, "resolved url");
    cJSON_Delete(catalog);
    cJSON_Delete(wanted);
    cJSON_Delete(state);
    free(body);
}

static int sent_etag(const char *heads, const char *etag) {
    char wanted[128];
    snprintf(wanted, sizeof wanted, "if-none-match: %s", etag);
    size_t length = strlen(heads);
    char *lowered = malloc(length + 1);
    for (size_t index = 0; index <= length; index++) {
        lowered[index] = (char)tolower((unsigned char)heads[index]);
    }
    int found = strstr(lowered, wanted) != NULL;
    free(lowered);
    return found;
}

static void it_asks_again_once_the_interval_has_passed_sending_the_etag(void) {
    start_scratch("again");
    check_once(200, "1.0.0", ETAG, START, "changed");
    served server;
    char *body = index_text("1.0.0");
    serve(&server, 200, body, ETAG);
    expect_kind(check_source("https", URL, LATER_25), "unchanged", "304");
    char *heads;
    CHECK(stop(&server, &heads) == 1);
    CHECK(sent_etag(heads, ETAG));
    free(heads);
    free(body);
    cJSON *state = state_of(URL);
    check_text(text_at(state, "checkedAt"), LATER_25, "checkedAt moved");
    cJSON_Delete(state);
}

static void it_answers_a_newer_index_as_changed_and_keeps_it(void) {
    start_scratch("newer");
    check_once(200, "1.0.0", NULL, START, "changed");
    served server;
    char *body = index_text("2.0.0");
    serve(&server, 200, body, NULL);
    catalog_answer answer = check_source("https", URL, LATER_25);
    check_text(kind_of(answer), "changed", "newer");
    check_text(text_at(value_at(answer.tree, "catalog/items/a"), "latest"), "2.0.0", "newer latest");
    cJSON_Delete(answer.tree);
    stop(&server, NULL);
    free(body);
    expect_current_latest("2.0.0", "kept newer");
}

static void it_leaves_checked_at_alone_when_a_check_fails(void) {
    start_scratch("fails");
    check_once(200, "1.0.0", ETAG, START, "changed");
    cJSON *before = state_of(URL);
    serve_nothing();
    catalog_answer failed = check_source("https", URL, LATER_25);
    check_text(kind_of(failed), "failed", "network down");
    const char *detail = text_at(failed.tree, "detail");
    CHECK(detail != NULL && strstr(detail, URL " could not be checked: ") == detail);
    cJSON_Delete(failed.tree);
    runtime_catalog_redirect_requests(NULL, NULL);
    cJSON *after = state_of(URL);
    CHECK(cJSON_Compare(before, after, 1));
    check_text(text_at(after, "checkedAt"), START, "checkedAt kept");
    cJSON_Delete(before);
    cJSON_Delete(after);
    served retry;
    char *body = index_text("1.0.0");
    serve(&retry, 200, body, ETAG);
    expect_kind(check_source("https", URL, LATER_26), "unchanged", "retry");
    CHECK(stop(&retry, NULL) == 1);
    free(body);
}

static void it_retries_on_the_next_check_after_a_first_check_that_failed(void) {
    start_scratch("retries");
    serve_nothing();
    expect_kind(check_source("https", URL, START), "failed", "first fails");
    runtime_catalog_redirect_requests(NULL, NULL);
    served retry;
    char *body = index_text("1.0.0");
    serve(&retry, 200, body, NULL);
    expect_kind(check_source("https", URL, LATER_1), "changed", "retried");
    CHECK(stop(&retry, NULL) == 1);
    free(body);
}

static void it_answers_an_index_the_reader_refuses_as_failed_and_keeps_the_previous_one(void) {
    start_scratch("refused");
    check_once(200, "1.0.0", NULL, START, "changed");
    served server;
    serve(&server, 200, "{", NULL);
    catalog_answer answer = check_source("https", URL, LATER_25);
    check_text(kind_of(answer), "failed", "refused index");
    const char *detail = text_at(answer.tree, "detail");
    CHECK(detail != NULL && strstr(detail, URL " served an index the host refuses: ") == detail);
    cJSON_Delete(answer.tree);
    stop(&server, NULL);
    expect_current_latest("1.0.0", "previous kept");
    cJSON *state = state_of(URL);
    check_text(text_at(state, "checkedAt"), START, "checkedAt kept on refusal");
    cJSON_Delete(state);
}

static void it_answers_a_304_with_no_kept_index_as_failed(void) {
    start_scratch("bare-304");
    check_once(304, NULL, NULL, START, "failed");
}

static void it_refuses_a_source_whose_url_is_not_https_without_asking(void) {
    start_scratch("not-https");
    served server;
    char *body = index_text("1.0.0");
    serve(&server, 200, body, NULL);
    runtime_catalog_redirect_requests("http://catalog.test", "http://127.0.0.1:1");
    expect_kind(check_source("https", "http://catalog.test/index.json", START), "failed", "plain http");
    CHECK(stop(&server, NULL) == 0);
    free(body);
}

static void it_keeps_an_https_index_without_its_byte_order_mark(void) {
    start_scratch("byte-order-mark");
    char *index = index_text("1.0.0");
    char *body = malloc(strlen(index) + 4);
    snprintf(body, strlen(index) + 4, "\xEF\xBB\xBF%s", index);
    served server;
    serve(&server, 200, body, NULL);
    expect_kind(check_source("https", URL, START), "changed", "with a byte order mark");
    stop(&server, NULL);
    cJSON *state = state_of(URL);
    check_text(text_at(state, "index"), index, "kept without the mark");
    cJSON_Delete(state);
    free(body);
    free(index);
}

static char *write_local(const char *name, const char *latest) {
    char directory[1200];
    snprintf(directory, sizeof directory, "%s/local", base);
    CHECK(spisor_make_directories(directory));
    char *path = malloc(1300);
    snprintf(path, 1300, "%s/%s", directory, name);
    char *text = index_text(latest);
    CHECK(spisor_replace_file(path, (const uint8_t *)text, strlen(text)));
    free(text);
    return path;
}

static void it_reads_a_local_index_from_disk_every_time(void) {
    start_scratch("local");
    char *path = write_local("catalog.json", "1.0.0");
    serve_nothing();
    expect_kind(check_source("local", path, START), "changed", "local first");
    expect_kind(check_source("local", path, LATER_1), "unchanged", "local again");
    free(write_local("catalog.json", "2.0.0"));
    catalog_answer changed = check_source("local", path, LATER_2);
    check_text(kind_of(changed), "changed", "local changed");
    check_text(text_at(value_at(changed.tree, "catalog/items/a"), "latest"), "2.0.0", "local latest");
    cJSON_Delete(changed.tree);
    runtime_catalog_redirect_requests(NULL, NULL);
    free(path);
}

static void it_answers_the_local_index_as_current_before_any_check(void) {
    start_scratch("local-current");
    char *path = write_local("catalog.json", "1.0.0");
    cJSON *sources = cJSON_CreateArray();
    cJSON_AddItemToArray(sources, source_json("local", path));
    cJSON *catalog = current_of(sources);
    check_text(text_at(value_at(catalog, "items/a"), "latest"), "1.0.0", "local current");
    cJSON_Delete(catalog);
    free(path);
}

static void it_answers_a_missing_local_index_as_failed(void) {
    start_scratch("local-missing");
    char path[1200];
    snprintf(path, sizeof path, "%s/absent.json", cache);
    expect_kind(check_source("local", path, START), "failed", "missing");
}

static void it_merges_every_readable_source_the_earlier_winning(void) {
    start_scratch("current-merge");
    char *first = write_local("first.json", "1.0.0");
    char *second = write_local("second.json", "2.0.0");
    cJSON *sources = cJSON_CreateArray();
    cJSON_AddItemToArray(sources, source_json("local", first));
    cJSON_AddItemToArray(sources, source_json("https", URL));
    cJSON_AddItemToArray(sources, source_json("local", second));
    cJSON *catalog = current_of(sources);
    check_text(text_at(value_at(catalog, "items/a"), "latest"), "1.0.0", "earlier wins");
    cJSON_Delete(catalog);
    free(first);
    free(second);
}

static void it_answers_an_empty_catalog_when_no_source_can_be_read(void) {
    start_scratch("current-empty");
    char path[1200];
    snprintf(path, sizeof path, "%s/absent.json", base);
    cJSON *sources = cJSON_CreateArray();
    cJSON_AddItemToArray(sources, source_json("local", path));
    cJSON *catalog = current_of(sources);
    const cJSON *items = value_at(catalog, "items");
    CHECK(cJSON_IsObject(items) && items->child == NULL);
    cJSON_Delete(catalog);
}

static void it_refuses_an_unusable_argument(void) {
    start_scratch("arguments");
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "source", source_json("https", URL));
    cJSON_AddStringToObject(argument, "cache", cache);
    cJSON_AddStringToObject(argument, "now", "yesterday");
    cJSON_AddNumberToObject(argument, "intervalHours", 24);
    catalog_answer answer = call_catalog(runtime_catalog_check, argument);
    expect_refusal(answer, "now", "now");
    cJSON_Delete(answer.tree);
    catalog_answer source = call_catalog(runtime_catalog_check, parsed("{\"source\":{\"kind\":\"ftp\"}}"));
    expect_refusal(source, "source", "source");
    cJSON_Delete(source.tree);
}

int main(void) {
    runtime_fetch_allow_plain_http(1);
    it_makes_one_request_for_two_checks_inside_the_interval();
    it_keeps_the_index_its_etag_and_the_time_under_the_catalog_folder();
    it_asks_again_once_the_interval_has_passed_sending_the_etag();
    it_answers_a_newer_index_as_changed_and_keeps_it();
    it_leaves_checked_at_alone_when_a_check_fails();
    it_retries_on_the_next_check_after_a_first_check_that_failed();
    it_answers_an_index_the_reader_refuses_as_failed_and_keeps_the_previous_one();
    it_answers_a_304_with_no_kept_index_as_failed();
    it_refuses_a_source_whose_url_is_not_https_without_asking();
    it_keeps_an_https_index_without_its_byte_order_mark();
    it_reads_a_local_index_from_disk_every_time();
    it_answers_the_local_index_as_current_before_any_check();
    it_answers_a_missing_local_index_as_failed();
    it_merges_every_readable_source_the_earlier_winning();
    it_answers_an_empty_catalog_when_no_source_can_be_read();
    it_refuses_an_unusable_argument();
    int status = check_summary("catalog_check_test");
    if (status == 0) {
        runtime_remove_tree(base);
    }
    return status;
}
