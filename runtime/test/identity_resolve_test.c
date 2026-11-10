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
#include "identity_resolve.h"

#define REPOSITORY(identifier, name_text, full_name_text) \
    "{\"id\":" identifier ",\"name\":\"" name_text "\",\"full_name\":\"" full_name_text "\",\"default_branch\":\"main\"}"
#define ROUTE(path, status, body) {path, status, NULL, body, sizeof body - 1, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0}
#define EMPTY_ROUTE(path, status) {path, status, NULL, "", 0, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0}

static const char found_seven[] = REPOSITORY("7", "n", "o/n");
static const char renamed_eight[] = REPOSITORY("8", "renamed", "o/renamed");
static const char named_nine[] = REPOSITORY("9", "n9", "o/n9");
static const char named_eleven[] = REPOSITORY("11", "taken", "o/taken");
static const char answers_thirty_one[] = REPOSITORY("31", "other", "o/other");
static const char answers_seven_for_007[] = REPOSITORY("7", "n", "o/n");
static const char beyond_exact_integers[] = REPOSITORY("9007199254740993", "big", "o/big");
static const char named_fifty[] = REPOSITORY("50", "n50", "o/n50");
static const char gitea_sixty[] = REPOSITORY("60", "tea", "o/tea");
static const char no_names[] = "{\"id\":70}";
static const char not_an_object[] = "[]";
static char oversized_body[300 * 1024];

static unsigned short server_port;

static cJSON *resolve_argument(const char *id, const char *last_known) {
    char api_base[128];
    snprintf(api_base, sizeof api_base, "http://127.0.0.1:%u", (unsigned)server_port);
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "id", id);
    if (last_known != NULL) {
        cJSON_AddStringToObject(argument, "lastKnown", last_known);
    }
    cJSON_AddStringToObject(argument, "apiBase", api_base);
    return argument;
}

static catalog_answer resolve(const char *id, const char *last_known) {
    return call_catalog(runtime_identity_resolve, resolve_argument(id, last_known));
}

static void expect_resolved(catalog_answer answer, const char *full_name, const char *label) {
    const char *found = text_at(answer.tree, "fullName");
    if (answer.status != SPISOR_STATUS_SUCCESS || found == NULL || strcmp(found, full_name) != 0) {
        fprintf(stderr, "%s: expected %s, got status %d and \"%s\"\n", label, full_name, (int)answer.status,
                found == NULL ? "(none)" : found);
        check_failures++;
    }
    cJSON_Delete(answer.tree);
}

static void expect_fix_naming(catalog_answer answer, const char *fragment, const char *label) {
    const char *fix = text_at(answer.tree, "fix");
    if (fix == NULL || strstr(fix, fragment) == NULL) {
        fprintf(stderr, "%s: the fix should name \"%s\", got \"%s\"\n", label, fragment, fix == NULL ? "(none)" : fix);
        check_failures++;
    }
}

static void expect_refused_then_free(catalog_answer answer, const char *fragment, const char *label) {
    CHECK(expect_refusal(answer, fragment, label));
    cJSON_Delete(answer.tree);
}

static void resolves_by_number(void) {
    catalog_answer answer = resolve("github:7", NULL);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    CHECK(strcmp(text_at(answer.tree, "name"), "n") == 0);
    CHECK(strcmp(text_at(answer.tree, "fullName"), "o/n") == 0);
    CHECK(strcmp(text_at(answer.tree, "defaultBranch"), "main") == 0);
    cJSON_Delete(answer.tree);
}

static void follows_a_rename(void) {
    expect_resolved(resolve("github:8", "o/old-name"), "o/renamed", "rename");
}

static void confirms_a_last_known_name_by_its_number(void) {
    expect_resolved(resolve("github:9", "o/n9"), "o/n9", "last known name");
}

static void confirms_by_name_when_the_host_wants_a_token_for_numbers(void) {
    expect_resolved(resolve("github:50", "o/n50"), "o/n50", "401 by number");
}

static void refuses_a_taken_name(void) {
    catalog_answer answer = resolve("github:10", "o/taken");
    CHECK(expect_refusal(answer, "o/taken", "taken name"));
    expect_fix_naming(answer, "10", "taken name asked");
    expect_fix_naming(answer, "11", "taken name found");
    cJSON_Delete(answer.tree);
}

static void says_rate_limited_on_403_and_429(void) {
    const char *ids[] = {"github:20", "github:21"};
    for (size_t index = 0; index < 2; index++) {
        catalog_answer answer = resolve(ids[index], NULL);
        CHECK(expect_refusal(answer, "rate limit", ids[index]));
        CHECK(strstr(text_at(answer.tree, "message"), "not found") == NULL);
        expect_fix_naming(answer, "wait", ids[index]);
        cJSON_Delete(answer.tree);
    }
}

static void says_rate_limited_even_with_a_last_known_name(void) {
    expect_refused_then_free(resolve("github:20", "o/n"), "rate limit", "limited with last known");
}

static void says_not_found_on_404_with_no_last_known_name(void) {
    expect_refused_then_free(resolve("github:22", NULL), "not found or is private", "404");
}

static void says_not_found_when_the_last_known_name_is_gone_too(void) {
    expect_refused_then_free(resolve("github:22", "o/vanished"), "not found", "both 404");
}

static void refuses_an_answer_whose_id_differs(void) {
    expect_refused_then_free(resolve("github:30", NULL), "31", "different id");
}

static void refuses_a_number_written_differently(void) {
    expect_refused_then_free(resolve("github:007", NULL), "7", "007 against 7");
}

static void refuses_a_number_beyond_what_a_double_holds(void) {
    expect_refused_then_free(resolve("github:9007199254740993", NULL), "exactly", "beyond a double");
}

static void refuses_an_answer_without_names(void) {
    expect_refused_then_free(resolve("github:70", NULL), "name", "no names");
}

static void refuses_a_last_known_name_that_is_not_owner_and_name(void) {
    expect_refused_then_free(resolve("github:9", "o/n9?x=1"), "lastKnown", "unsafe last known");
}

static void reads_a_gitea_host_through_its_api_root(void) {
    char api_root[128];
    snprintf(api_root, sizeof api_root, "http://127.0.0.1:%u/api/v1", (unsigned)server_port);
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "id", "gitea@git.example.com:60");
    cJSON_AddStringToObject(argument, "apiBase", api_root);
    expect_resolved(call_catalog(runtime_identity_resolve, argument), "o/tea", "gitea");
}

static void refuses_a_gitea_id_with_no_host(void) {
    catalog_answer answer = resolve("gitea:60", NULL);
    CHECK(expect_refusal(answer, "host", "gitea without host"));
    expect_fix_naming(answer, "gitea@", "gitea without host");
    cJSON_Delete(answer.tree);
}

static void names_the_missing_resolver_for_an_unknown_scheme(void) {
    catalog_answer answer = resolve("mercurial:5", NULL);
    CHECK(expect_refusal(answer, "mercurial", "unknown scheme"));
    expect_fix_naming(answer, "plugin", "unknown scheme");
    cJSON_Delete(answer.tree);
}

static void refuses_an_invalid_id(void) {
    expect_refused_then_free(resolve("calendar", NULL), "not valid", "invalid id");
}

static void resolves_local_without_the_network(void) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "id", "local:plugin-stub");
    cJSON_AddStringToObject(argument, "apiBase", "http://127.0.0.1:1");
    catalog_answer answer = call_catalog(runtime_identity_resolve, argument);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    CHECK(strcmp(text_at(answer.tree, "name"), "plugin-stub") == 0);
    CHECK(strcmp(text_at(answer.tree, "fullName"), "plugin-stub") == 0);
    CHECK(cJSON_GetObjectItemCaseSensitive(answer.tree, "defaultBranch") == NULL);
    cJSON_Delete(answer.tree);
}

static void refuses_a_body_above_the_cap(void) {
    expect_refused_then_free(resolve("github:80", NULL), "more than a lookup reads", "oversized body");
}

static void refuses_a_body_that_is_not_an_object(void) {
    expect_refused_then_free(resolve("github:81", NULL), "did not answer a repository", "array body");
}

static void run_against_the_server(void) {
    memset(oversized_body, 'a', sizeof oversized_body);
    const http_route routes[] = {
        ROUTE("/repositories/7", 200, found_seven),
        ROUTE("/repositories/8", 200, renamed_eight),
        EMPTY_ROUTE("/repositories/9", 404),
        ROUTE("/repos/o/n9", 200, named_nine),
        EMPTY_ROUTE("/repositories/10", 404),
        ROUTE("/repos/o/taken", 200, named_eleven),
        EMPTY_ROUTE("/repositories/20", 403),
        EMPTY_ROUTE("/repositories/21", 429),
        EMPTY_ROUTE("/repositories/22", 404),
        ROUTE("/repositories/30", 200, answers_thirty_one),
        ROUTE("/repositories/007", 200, answers_seven_for_007),
        ROUTE("/repositories/9007199254740993", 200, beyond_exact_integers),
        EMPTY_ROUTE("/repositories/50", 401),
        ROUTE("/repos/o/n50", 200, named_fifty),
        ROUTE("/api/v1/repositories/60", 200, gitea_sixty),
        ROUTE("/repositories/70", 200, no_names),
        {"/repositories/80", 200, NULL, oversized_body, sizeof oversized_body, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0},
        ROUTE("/repositories/81", 200, not_an_object),
    };
    http_fixture *server = http_fixture_start(routes, sizeof routes / sizeof routes[0]);
    CHECK(server != NULL);
    if (server == NULL) {
        return;
    }
    server_port = http_fixture_port(server);
    resolves_by_number();
    follows_a_rename();
    confirms_a_last_known_name_by_its_number();
    confirms_by_name_when_the_host_wants_a_token_for_numbers();
    refuses_a_taken_name();
    says_rate_limited_on_403_and_429();
    says_rate_limited_even_with_a_last_known_name();
    says_not_found_on_404_with_no_last_known_name();
    says_not_found_when_the_last_known_name_is_gone_too();
    refuses_an_answer_whose_id_differs();
    refuses_a_number_written_differently();
    refuses_a_number_beyond_what_a_double_holds();
    refuses_an_answer_without_names();
    refuses_a_last_known_name_that_is_not_owner_and_name();
    refuses_a_body_above_the_cap();
    refuses_a_body_that_is_not_an_object();
    reads_a_gitea_host_through_its_api_root();
    refuses_a_gitea_id_with_no_host();
    names_the_missing_resolver_for_an_unknown_scheme();
    refuses_an_invalid_id();
    resolves_local_without_the_network();
    free(http_fixture_stop(server));
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
    return check_summary("identity_resolve_test");
}
