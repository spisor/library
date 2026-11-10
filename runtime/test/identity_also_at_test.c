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
#include "identity_also_at.h"

#define REPOSITORY(identifier, name_text, full_name_text) \
    "{\"id\":" identifier ",\"name\":\"" name_text "\",\"full_name\":\"" full_name_text "\",\"default_branch\":\"main\"}"
#define ROUTE(path, status, body) {path, status, NULL, body, sizeof body - 1, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0}
#define EMPTY_ROUTE(path, status) {path, status, NULL, "", 0, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0}
#define FILE_ROUTE(repository, body) ROUTE("/o/" repository "/HEAD/.weivel/also-at.json", 200, body)

static unsigned short server_port;

static catalog_answer agree(const char *id, const char *files_json) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "id", id);
    cJSON_AddItemToObject(argument, "files", parsed(files_json));
    return call_catalog(runtime_identity_agree, argument);
}

static catalog_answer also_at(const char *id) {
    char api_base[128];
    snprintf(api_base, sizeof api_base, "http://127.0.0.1:%u", (unsigned)server_port);
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "id", id);
    cJSON_AddStringToObject(argument, "apiBase", api_base);
    return call_catalog(runtime_identity_also_at, argument);
}

static catalog_answer accepts_version(const char *recorded, const char *primary) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "recorded", recorded);
    cJSON_AddStringToObject(argument, "primary", primary);
    return call_catalog(runtime_identity_accepts_version, argument);
}

static int ids_are(const catalog_answer answer, const char *expected) {
    const cJSON *ids = cJSON_GetObjectItemCaseSensitive(answer.tree, "ids");
    char *printed = cJSON_IsArray(ids) ? cJSON_PrintUnformatted(ids) : NULL;
    int matched = printed != NULL && strcmp(printed, expected) == 0;
    if (!matched) {
        fprintf(stderr, "expected ids %s, got %s\n", expected, printed == NULL ? "(none)" : printed);
        check_failures++;
    }
    cJSON_free(printed);
    return matched;
}

static int flag_is(const catalog_answer answer, const char *member, int expected) {
    const cJSON *flag = cJSON_GetObjectItemCaseSensitive(answer.tree, member);
    int matched = expected < 0 ? flag == NULL : cJSON_IsBool(flag) && cJSON_IsTrue(flag) == expected;
    if (!matched) {
        fprintf(stderr, "expected %s to be %d\n", member, expected);
        check_failures++;
    }
    return matched;
}

static int primary_is(const catalog_answer answer, const char *expected) {
    const char *primary = text_at(answer.tree, "primary");
    int matched = primary != NULL && strcmp(primary, expected) == 0;
    if (!matched) {
        fprintf(stderr, "expected primary %s, got %s\n", expected, primary == NULL ? "(none)" : primary);
        check_failures++;
    }
    return matched;
}

#define BOTH_LIST_EACH_OTHER                                                                   \
    "{\"github:1\":{\"primary\":\"github:1\",\"ids\":[\"github:1\",\"github:2\"]},"           \
    "\"github:2\":{\"primary\":\"github:1\",\"ids\":[\"github:1\",\"github:2\"]}}"

static void two_copies_listing_each_other_agree(void) {
    catalog_answer answer = agree("github:1", BOTH_LIST_EACH_OTHER);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    CHECK(ids_are(answer, "[\"github:1\",\"github:2\"]"));
    CHECK(flag_is(answer, "startFileMalformed", -1));
    cJSON_Delete(answer.tree);
}

static void agreement_does_not_depend_on_which_copy_it_starts_from(void) {
    catalog_answer answer = agree("github:2", BOTH_LIST_EACH_OTHER);
    CHECK(ids_are(answer, "[\"github:2\",\"github:1\"]"));
    CHECK(primary_is(answer, "github:1"));
    cJSON_Delete(answer.tree);
}

static void a_one_sided_claim_is_not_counted(void) {
    catalog_answer answer = agree("github:1",
                                  "{\"github:1\":{\"primary\":\"github:1\",\"ids\":[\"github:1\",\"github:2\"]},"
                                  "\"github:2\":{\"primary\":\"github:2\",\"ids\":[\"github:2\"]}}");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    CHECK(ids_are(answer, "[\"github:1\"]"));
    cJSON_Delete(answer.tree);
}

static void a_copy_that_lists_us_but_is_not_listed_by_us_is_not_counted(void) {
    catalog_answer answer = agree("github:1",
                                  "{\"github:1\":{\"primary\":\"github:1\",\"ids\":[\"github:1\"]},"
                                  "\"github:2\":{\"primary\":\"github:1\",\"ids\":[\"github:1\",\"github:2\"]}}");
    CHECK(ids_are(answer, "[\"github:1\"]"));
    cJSON_Delete(answer.tree);
}

static void an_unreachable_copy_is_left_out_and_is_no_refusal(void) {
    catalog_answer answer = agree("github:1",
                                  "{\"github:1\":{\"primary\":\"github:1\",\"ids\":[\"github:1\",\"github:2\",\"github:3\"]},"
                                  "\"github:2\":null,"
                                  "\"github:3\":{\"primary\":\"github:1\",\"ids\":[\"github:1\",\"github:3\"]}}");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    CHECK(ids_are(answer, "[\"github:1\",\"github:3\"]"));
    cJSON_Delete(answer.tree);
}

static void a_copy_with_no_entry_at_all_is_left_out_too(void) {
    catalog_answer answer =
        agree("github:1", "{\"github:1\":{\"primary\":\"github:1\",\"ids\":[\"github:1\",\"github:2\"]}}");
    CHECK(ids_are(answer, "[\"github:1\"]"));
    cJSON_Delete(answer.tree);
}

static void no_also_at_file_means_only_the_id_itself(void) {
    catalog_answer answer = agree("github:1", "{\"github:1\":null}");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    CHECK(ids_are(answer, "[\"github:1\"]"));
    CHECK(primary_is(answer, "github:1"));
    CHECK(flag_is(answer, "primaryConfirmed", 1));
    CHECK(flag_is(answer, "startFileMalformed", -1));
    cJSON_Delete(answer.tree);
}

static void the_primary_is_the_one_both_name(void) {
    catalog_answer answer = agree("github:2",
                                  "{\"github:2\":{\"primary\":\"github:1\",\"ids\":[\"github:2\",\"github:1\"]},"
                                  "\"github:1\":{\"primary\":\"github:1\",\"ids\":[\"github:1\",\"github:2\"]}}");
    CHECK(primary_is(answer, "github:1"));
    CHECK(flag_is(answer, "primaryConfirmed", 1));
    cJSON_Delete(answer.tree);
}

static void a_primary_that_could_not_be_confirmed_says_so(void) {
    catalog_answer answer = agree("github:2",
                                  "{\"github:2\":{\"primary\":\"github:1\",\"ids\":[\"github:2\",\"github:1\"]},"
                                  "\"github:1\":null}");
    CHECK(primary_is(answer, "github:1"));
    CHECK(flag_is(answer, "primaryConfirmed", 0));
    CHECK(ids_are(answer, "[\"github:2\"]"));
    cJSON_Delete(answer.tree);
}

static void disagreeing_primaries_are_refused_with_a_fix(void) {
    catalog_answer answer = agree("github:1",
                                  "{\"github:1\":{\"primary\":\"github:1\",\"ids\":[\"github:1\",\"github:2\"]},"
                                  "\"github:2\":{\"primary\":\"github:2\",\"ids\":[\"github:1\",\"github:2\"]}}");
    CHECK(expect_refusal(answer, "primaries", "disagreeing primaries"));
    CHECK(strstr(text_at(answer.tree, "fix"), "same primary") != NULL);
    cJSON_Delete(answer.tree);
}

static void an_id_outside_the_grammar_is_left_out(void) {
    catalog_answer answer = agree("github:1",
                                  "{\"github:1\":{\"primary\":\"github:1\",\"ids\":[\"github:1\",\"github:../x\",\"github:2\"]},"
                                  "\"github:2\":{\"primary\":\"github:1\",\"ids\":[\"github:1\",\"github:2\"]}}");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    CHECK(ids_are(answer, "[\"github:1\"]"));
    cJSON_Delete(answer.tree);
}

static void a_start_file_that_could_not_be_read_is_not_a_confirmed_primary(void) {
    catalog_answer answer = agree("github:1", "{\"github:1\":false}");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    CHECK(ids_are(answer, "[\"github:1\"]"));
    CHECK(primary_is(answer, "github:1"));
    CHECK(flag_is(answer, "primaryConfirmed", 0));
    CHECK(flag_is(answer, "startFileUnavailable", 1));
    CHECK(flag_is(answer, "startFileMalformed", -1));
    cJSON_Delete(answer.tree);
}

static void a_listed_copy_that_could_not_be_read_is_left_out(void) {
    catalog_answer answer = agree("github:1",
                                  "{\"github:1\":{\"primary\":\"github:1\",\"ids\":[\"github:1\",\"github:2\"]},"
                                  "\"github:2\":false}");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    CHECK(ids_are(answer, "[\"github:1\"]"));
    CHECK(flag_is(answer, "startFileUnavailable", -1));
    cJSON_Delete(answer.tree);
}

static void a_malformed_start_file_is_reported_in_the_answer(void) {
    const char *malformed[] = {"{\"github:1\":{\"primary\":\"nonsense\",\"ids\":[\"github:1\"]}}",
                               "{\"github:1\":{\"primary\":\"github:1\"}}",
                               "{\"github:1\":{\"primary\":\"github:1\",\"ids\":\"github:1\"}}",
                               "{\"github:1\":{\"primary\":\"github:1\",\"ids\":[\"local:folder\"]}}",
                               "{\"github:1\":\"not an also-at file\"}"};
    for (size_t index = 0; index < sizeof malformed / sizeof malformed[0]; index++) {
        catalog_answer answer = agree("github:1", malformed[index]);
        CHECK(answer.status == SPISOR_STATUS_SUCCESS);
        CHECK(ids_are(answer, "[\"github:1\"]"));
        CHECK(flag_is(answer, "startFileMalformed", 1));
        cJSON_Delete(answer.tree);
    }
}

static void a_malformed_file_on_another_copy_leaves_only_that_copy_out(void) {
    catalog_answer answer = agree("github:1",
                                  "{\"github:1\":{\"primary\":\"github:1\",\"ids\":[\"github:1\",\"github:2\",\"github:3\"]},"
                                  "\"github:2\":{\"primary\":7,\"ids\":[\"github:1\",\"github:2\"]},"
                                  "\"github:3\":{\"primary\":\"github:1\",\"ids\":[\"github:1\",\"github:3\"]}}");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    CHECK(ids_are(answer, "[\"github:1\",\"github:3\"]"));
    CHECK(flag_is(answer, "startFileMalformed", -1));
    cJSON_Delete(answer.tree);
}

static void a_file_listing_more_than_sixteen_ids_is_malformed(void) {
    char text[1024] = "{\"github:1\":{\"primary\":\"github:1\",\"ids\":[\"github:1\"";
    for (int number = 2; number <= 17; number++) {
        char more[32];
        snprintf(more, sizeof more, ",\"github:%d\"", number);
        strcat(text, more);
    }
    strcat(text, "]}}");
    catalog_answer answer = agree("github:1", text);
    CHECK(ids_are(answer, "[\"github:1\"]"));
    CHECK(flag_is(answer, "startFileMalformed", 1));
    cJSON_Delete(answer.tree);
}

static void sixteen_ids_are_allowed(void) {
    char text[2048] = "{\"github:1\":{\"primary\":\"github:1\",\"ids\":[\"github:1\"";
    for (int number = 2; number <= 16; number++) {
        char more[32];
        snprintf(more, sizeof more, ",\"github:%d\"", number);
        strcat(text, more);
    }
    strcat(text, "]}}");
    catalog_answer answer = agree("github:1", text);
    CHECK(flag_is(answer, "startFileMalformed", -1));
    cJSON_Delete(answer.tree);
}

static void refuses_a_start_id_that_is_not_a_repo(void) {
    const char *refused[] = {"calendar", "local:folder", "github:1/kernel"};
    for (size_t index = 0; index < sizeof refused / sizeof refused[0]; index++) {
        catalog_answer answer = agree(refused[index], "{}");
        CHECK(expect_refusal(answer, "id", refused[index]));
        cJSON_Delete(answer.tree);
    }
}

static void a_version_from_the_recorded_primary_is_accepted(void) {
    catalog_answer answer = accepts_version("github:1", "github:1");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    CHECK(flag_is(answer, "accepted", 1));
    CHECK(flag_is(answer, "needsUserAcceptance", 0));
    cJSON_Delete(answer.tree);
}

static void a_new_primary_needs_the_user_to_accept_it(void) {
    catalog_answer answer = accepts_version("github:1", "gitea@git.example.com:5");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    CHECK(flag_is(answer, "accepted", 0));
    CHECK(flag_is(answer, "needsUserAcceptance", 1));
    cJSON_Delete(answer.tree);
}

static void a_version_check_refuses_an_id_outside_the_grammar(void) {
    catalog_answer answer = accepts_version("github:1", "github:1/../x");
    CHECK(expect_refusal(answer, "primary", "bad primary"));
    cJSON_Delete(answer.tree);
    answer = accepts_version("", "github:1");
    CHECK(expect_refusal(answer, "recorded", "bad recorded"));
    cJSON_Delete(answer.tree);
}

static void reads_the_files_of_the_copies_its_own_file_lists(void) {
    catalog_answer answer = also_at("github:1");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    CHECK(ids_are(answer, "[\"github:1\",\"github:2\"]"));
    CHECK(primary_is(answer, "github:1"));
    cJSON_Delete(answer.tree);
}

static void leaves_out_a_copy_the_host_cannot_find(void) {
    catalog_answer answer = also_at("github:10");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    CHECK(ids_are(answer, "[\"github:10\",\"github:11\"]"));
    cJSON_Delete(answer.tree);
}

static void leaves_out_a_copy_that_is_a_one_sided_claim(void) {
    catalog_answer answer = also_at("github:20");
    CHECK(ids_are(answer, "[\"github:20\"]"));
    cJSON_Delete(answer.tree);
}

static void does_not_follow_ids_beyond_the_set_its_own_file_lists(void) {
    catalog_answer answer = also_at("github:30");
    CHECK(ids_are(answer, "[\"github:30\",\"github:31\"]"));
    cJSON_Delete(answer.tree);
}

static void answers_only_the_id_when_it_has_no_file(void) {
    catalog_answer answer = also_at("github:40");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    CHECK(ids_are(answer, "[\"github:40\"]"));
    CHECK(flag_is(answer, "startFileMalformed", -1));
    cJSON_Delete(answer.tree);
}

static void reports_a_malformed_file_of_the_id_itself(void) {
    const char *ids[] = {"github:50", "github:51"};
    for (size_t index = 0; index < 2; index++) {
        catalog_answer answer = also_at(ids[index]);
        CHECK(answer.status == SPISOR_STATUS_SUCCESS);
        CHECK(ids_are(answer, index == 0 ? "[\"github:50\"]" : "[\"github:51\"]"));
        CHECK(flag_is(answer, "startFileMalformed", 1));
        cJSON_Delete(answer.tree);
    }
}

static void says_so_when_the_start_copy_answers_a_server_error(void) {
    catalog_answer answer = also_at("github:80");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    CHECK(ids_are(answer, "[\"github:80\"]"));
    CHECK(flag_is(answer, "primaryConfirmed", 0));
    CHECK(flag_is(answer, "startFileUnavailable", 1));
    cJSON_Delete(answer.tree);
}

static void leaves_out_a_listed_copy_that_answers_a_server_error(void) {
    catalog_answer answer = also_at("github:81");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    CHECK(ids_are(answer, "[\"github:81\"]"));
    CHECK(flag_is(answer, "primaryConfirmed", 1));
    CHECK(flag_is(answer, "startFileUnavailable", -1));
    cJSON_Delete(answer.tree);
}

static void does_not_keep_a_file_when_the_name_changed_hands_during_the_read(void) {
    catalog_answer answer = also_at("github:90");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    CHECK(ids_are(answer, "[\"github:90\"]"));
    CHECK(flag_is(answer, "startFileUnavailable", 1));
    CHECK(flag_is(answer, "primaryConfirmed", 0));
    cJSON_Delete(answer.tree);
}

static void refuses_when_the_id_itself_cannot_be_resolved(void) {
    catalog_answer answer = also_at("github:60");
    CHECK(expect_refusal(answer, "not found", "unresolvable start"));
    cJSON_Delete(answer.tree);
}

static void reads_a_gitea_copy_from_its_raw_file_route(void) {
    char api_root[128];
    snprintf(api_root, sizeof api_root, "http://127.0.0.1:%u/api/v1", (unsigned)server_port);
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "id", "gitea@git.example.com:70");
    cJSON_AddStringToObject(argument, "apiBase", api_root);
    catalog_answer answer = call_catalog(runtime_identity_also_at, argument);
    CHECK(ids_are(answer, "[\"gitea@git.example.com:70\",\"gitea@git.example.com:71\"]"));
    cJSON_Delete(answer.tree);
}

static void refuses_an_apibase_that_is_not_a_string(void) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "id", "github:1");
    cJSON_AddNumberToObject(argument, "apiBase", 5);
    catalog_answer answer = call_catalog(runtime_identity_also_at, argument);
    CHECK(expect_refusal(answer, "apiBase", "numeric apiBase"));
    cJSON_Delete(answer.tree);
}

static void run_against_the_server(void) {
    static const char first_file[] =
        "{\"primary\":\"github:1\",\"ids\":[\"github:1\",\"github:2\"]}";
    static const char repository_one[] = REPOSITORY("1", "n1", "o/n1");
    static const char repository_two[] = REPOSITORY("2", "n2", "o/n2");
    static const char repository_ten[] = REPOSITORY("10", "n10", "o/n10");
    static const char repository_eleven[] = REPOSITORY("11", "n11", "o/n11");
    static const char repository_twenty[] = REPOSITORY("20", "n20", "o/n20");
    static const char repository_twenty_one[] = REPOSITORY("21", "n21", "o/n21");
    static const char repository_thirty[] = REPOSITORY("30", "n30", "o/n30");
    static const char repository_thirty_one[] = REPOSITORY("31", "n31", "o/n31");
    static const char repository_thirty_two[] = REPOSITORY("32", "n32", "o/n32");
    static const char repository_forty[] = REPOSITORY("40", "n40", "o/n40");
    static const char repository_fifty[] = REPOSITORY("50", "n50", "o/n50");
    static const char repository_fifty_one[] = REPOSITORY("51", "n51", "o/n51");
    static const char ten_file[] = "{\"primary\":\"github:10\",\"ids\":[\"github:10\",\"github:11\",\"github:12\"]}";
    static const char eleven_file[] = "{\"primary\":\"github:10\",\"ids\":[\"github:10\",\"github:11\"]}";
    static const char twenty_file[] = "{\"primary\":\"github:20\",\"ids\":[\"github:20\",\"github:21\"]}";
    static const char twenty_one_file[] = "{\"primary\":\"github:21\",\"ids\":[\"github:21\"]}";
    static const char thirty_file[] = "{\"primary\":\"github:30\",\"ids\":[\"github:30\",\"github:31\"]}";
    static const char thirty_one_file[] = "{\"primary\":\"github:30\",\"ids\":[\"github:30\",\"github:31\",\"github:32\"]}";
    static const char thirty_two_file[] = "{\"primary\":\"github:30\",\"ids\":[\"github:30\",\"github:32\"]}";
    static const char fifty_file[] = "not json";
    static const char fifty_one_file[] = "{\"primary\":\"github:51\",\"ids\":[\"../../etc\"]}";
    static const char gitea_seventy[] = REPOSITORY("70", "tea70", "o/tea70");
    static const char gitea_seventy_one[] = REPOSITORY("71", "tea71", "o/tea71");
    static const char gitea_seventy_file[] =
        "{\"primary\":\"gitea@git.example.com:70\",\"ids\":[\"gitea@git.example.com:70\",\"gitea@git.example.com:71\"]}";
    static const char repository_eighty[] = REPOSITORY("80", "n80", "o/n80");
    static const char repository_eighty_one[] = REPOSITORY("81", "n81", "o/n81");
    static const char repository_eighty_two[] = REPOSITORY("82", "n82", "o/n82");
    static const char repository_ninety[] = REPOSITORY("90", "n90", "o/n90");
    static const char repository_ninety_taken[] = REPOSITORY("90", "taken", "o/taken");
    static const char eighty_one_file[] = "{\"primary\":\"github:81\",\"ids\":[\"github:81\",\"github:82\"]}";
    static const char ninety_file[] = "{\"primary\":\"github:90\",\"ids\":[\"github:90\"]}";
    const http_route routes[] = {
        ROUTE("/repositories/1", 200, repository_one),
        ROUTE("/repositories/2", 200, repository_two),
        FILE_ROUTE("n1", first_file),
        FILE_ROUTE("n2", first_file),
        ROUTE("/repositories/10", 200, repository_ten),
        ROUTE("/repositories/11", 200, repository_eleven),
        EMPTY_ROUTE("/repositories/12", 404),
        FILE_ROUTE("n10", ten_file),
        FILE_ROUTE("n11", eleven_file),
        ROUTE("/repositories/20", 200, repository_twenty),
        ROUTE("/repositories/21", 200, repository_twenty_one),
        FILE_ROUTE("n20", twenty_file),
        FILE_ROUTE("n21", twenty_one_file),
        ROUTE("/repositories/30", 200, repository_thirty),
        ROUTE("/repositories/31", 200, repository_thirty_one),
        ROUTE("/repositories/32", 200, repository_thirty_two),
        FILE_ROUTE("n30", thirty_file),
        FILE_ROUTE("n31", thirty_one_file),
        FILE_ROUTE("n32", thirty_two_file),
        ROUTE("/repositories/40", 200, repository_forty),
        EMPTY_ROUTE("/o/n40/HEAD/.weivel/also-at.json", 404),
        ROUTE("/repositories/50", 200, repository_fifty),
        ROUTE("/repositories/51", 200, repository_fifty_one),
        FILE_ROUTE("n50", fifty_file),
        FILE_ROUTE("n51", fifty_one_file),
        EMPTY_ROUTE("/repositories/60", 404),
        ROUTE("/repositories/80", 200, repository_eighty),
        EMPTY_ROUTE("/o/n80/HEAD/.weivel/also-at.json", 500),
        ROUTE("/repositories/81", 200, repository_eighty_one),
        ROUTE("/repositories/82", 200, repository_eighty_two),
        FILE_ROUTE("n81", eighty_one_file),
        EMPTY_ROUTE("/o/n82/HEAD/.weivel/also-at.json", 500),
        {"/repositories/90", 200, NULL, repository_ninety, sizeof repository_ninety - 1, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 1},
        ROUTE("/repositories/90", 200, repository_ninety_taken),
        FILE_ROUTE("n90", ninety_file),
        ROUTE("/api/v1/repositories/70", 200, gitea_seventy),
        ROUTE("/api/v1/repositories/71", 200, gitea_seventy_one),
        ROUTE("/api/v1/repos/o/tea70/raw/.weivel/also-at.json", 200, gitea_seventy_file),
        ROUTE("/api/v1/repos/o/tea71/raw/.weivel/also-at.json", 200, gitea_seventy_file),
    };
    http_fixture *server = http_fixture_start(routes, sizeof routes / sizeof routes[0]);
    CHECK(server != NULL);
    if (server == NULL) {
        return;
    }
    server_port = http_fixture_port(server);
    reads_the_files_of_the_copies_its_own_file_lists();
    leaves_out_a_copy_the_host_cannot_find();
    leaves_out_a_copy_that_is_a_one_sided_claim();
    does_not_follow_ids_beyond_the_set_its_own_file_lists();
    answers_only_the_id_when_it_has_no_file();
    reports_a_malformed_file_of_the_id_itself();
    says_so_when_the_start_copy_answers_a_server_error();
    leaves_out_a_listed_copy_that_answers_a_server_error();
    does_not_keep_a_file_when_the_name_changed_hands_during_the_read();
    refuses_when_the_id_itself_cannot_be_resolved();
    reads_a_gitea_copy_from_its_raw_file_route();
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
    two_copies_listing_each_other_agree();
    agreement_does_not_depend_on_which_copy_it_starts_from();
    a_one_sided_claim_is_not_counted();
    a_copy_that_lists_us_but_is_not_listed_by_us_is_not_counted();
    an_unreachable_copy_is_left_out_and_is_no_refusal();
    a_copy_with_no_entry_at_all_is_left_out_too();
    no_also_at_file_means_only_the_id_itself();
    the_primary_is_the_one_both_name();
    a_primary_that_could_not_be_confirmed_says_so();
    disagreeing_primaries_are_refused_with_a_fix();
    an_id_outside_the_grammar_is_left_out();
    a_start_file_that_could_not_be_read_is_not_a_confirmed_primary();
    a_listed_copy_that_could_not_be_read_is_left_out();
    a_malformed_start_file_is_reported_in_the_answer();
    a_malformed_file_on_another_copy_leaves_only_that_copy_out();
    a_file_listing_more_than_sixteen_ids_is_malformed();
    sixteen_ids_are_allowed();
    refuses_a_start_id_that_is_not_a_repo();
    a_version_from_the_recorded_primary_is_accepted();
    a_new_primary_needs_the_user_to_accept_it();
    a_version_check_refuses_an_id_outside_the_grammar();
    refuses_an_apibase_that_is_not_a_string();
    run_against_the_server();
    return check_summary("identity_also_at_test");
}
