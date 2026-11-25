#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "spisor/allocation.h"
#include "definition_fixture.h"
#include "http_fixture.h"
#include "runtime/identity.h"
#include "transport_hooks.h"

#define REPOSITORY(identifier, name_text, full_name_text) \
    "{\"id\":" identifier ",\"name\":\"" name_text "\",\"full_name\":\"" full_name_text "\",\"default_branch\":\"main\"}"
#define ROUTE(path, status, body) {path, status, NULL, body, sizeof body - 1, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0}
#define EMPTY_ROUTE(path, status) {path, status, NULL, "", 0, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0}
#define FILE_ROUTE(repository, body) ROUTE("/o/" repository "/HEAD/.weivel/also-at.json", 200, body)

static unsigned short server_port;

/** One agree or also_at call's status, answer and refusal, released by release_call. */
typedef struct agreement_call {
    spisor_status status;
    runtime_identity_agreement answer;
    spisor_refusal refusal;
} agreement_call;

static const runtime_identity_definition *identity(void) {
    return runtime_identity_definition_of();
}

static void release_call(agreement_call *call) {
    identity()->release_agreement(identity()->context, &call->answer);
    identity()->release_refusal(identity()->context, &call->refusal);
}

/** The copies a test's files object describes, as a reader would hand them: false a failed read, null no file. */
static runtime_identity_copy *copies_of(const cJSON *files, spisor_value_tree *trees, size_t *count) {
    runtime_identity_copy *copies = spisor_memory_allocate_zeroed((size_t)cJSON_GetArraySize(files) + 1,
            sizeof *copies);
    const cJSON *file;
    *count = 0;
    cJSON_ArrayForEach(file, files) {
        runtime_identity_copy *copy = &copies[(*count)++];
        copy->id = file->string;
        copy->read_failed = cJSON_IsFalse(file) ? 1 : 0;
        if (!cJSON_IsFalse(file) && !cJSON_IsNull(file)) {
            cJSON *wrapper = cJSON_CreateObject();
            cJSON_AddItemReferenceToObject(wrapper, "value", (cJSON *)file);
            CHECK(spisor_vector_value(wrapper, "value", &trees[*count - 1]));
            cJSON_Delete(wrapper);
            copy->file = &trees[*count - 1].root;
        }
    }
    return copies;
}

static agreement_call agree(const char *id, const char *files_json) {
    cJSON *files = spisor_parse_json_reporting((const uint8_t *)files_json, strlen(files_json), &(int){0});
    size_t file_count = (size_t)cJSON_GetArraySize(files);
    spisor_value_tree *trees = spisor_memory_allocate_zeroed(file_count + 1, sizeof *trees);
    runtime_identity_agree_argument argument = SPISOR_INIT(runtime_identity_agree_argument);
    agreement_call call = {SPISOR_OK, SPISOR_INIT(runtime_identity_agreement), SPISOR_INIT(spisor_refusal)};
    for (size_t index = 0; index < file_count; index++) {
        trees[index].size = (uint32_t)sizeof trees[index];
    }
    runtime_identity_copy *copies = copies_of(files, trees, &argument.copy_count);
    argument.id = id;
    argument.copies = copies;
    call.status = identity()->agree(identity()->context, &argument, &call.answer, &call.refusal);
    for (size_t index = 0; index < file_count; index++) {
        spisor_value_release(&trees[index]);
    }
    spisor_memory_release(trees);
    spisor_memory_release(copies);
    cJSON_Delete(files);
    return call;
}

static agreement_call also_at_root(const char *api_base, const char *id) {
    runtime_identity_also_at_argument argument = SPISOR_INIT(runtime_identity_also_at_argument);
    agreement_call call = {SPISOR_OK, SPISOR_INIT(runtime_identity_agreement), SPISOR_INIT(spisor_refusal)};
    argument.id = id;
    argument.api_base = api_base;
    call.status = identity()->also_at(identity()->context, &argument, &call.answer, &call.refusal);
    return call;
}

static agreement_call also_at(const char *id) {
    char api_base[128];
    snprintf(api_base, sizeof api_base, "http://127.0.0.1:%u", (unsigned)server_port);
    return also_at_root(api_base, id);
}

/** The ids as the JSON array text a test expects, so each expectation reads as the set it names. */
static int ids_are(const agreement_call answer, const char *expected) {
    char printed[1024] = "[";
    for (size_t index = 0; index < answer.answer.id_count; index++) {
        strcat(printed, index == 0 ? "\"" : ",\"");
        strcat(printed, answer.answer.ids[index]);
        strcat(printed, "\"");
    }
    strcat(printed, "]");
    int matched = answer.status == SPISOR_OK && strcmp(printed, expected) == 0;
    if (!matched) {
        fprintf(stderr, "expected ids %s, got status %d and %s\n", expected, (int)answer.status, printed);
        check_failures++;
    }
    return matched;
}

static int flag_is(const agreement_call answer, const char *member, int expected) {
    uint32_t flag = strcmp(member, "primaryConfirmed") == 0       ? answer.answer.primary_confirmed
                    : strcmp(member, "startFileMalformed") == 0   ? answer.answer.start_file_malformed
                    : strcmp(member, "startFileUnavailable") == 0 ? answer.answer.start_file_unavailable
                                                                  : 2;
    int matched = answer.status == SPISOR_OK && flag == (uint32_t)expected;
    if (!matched) {
        fprintf(stderr, "expected %s to be %d\n", member, expected);
        check_failures++;
    }
    return matched;
}

static int expect_refusal(const agreement_call answer, const char *fragment, const char *label) {
    return check_refused(answer.status, SPISOR_REFUSED, &answer.refusal, fragment, label);
}

static int primary_is(const agreement_call answer, const char *expected) {
    const char *primary = answer.answer.primary;
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
    agreement_call answer = agree("github:1", BOTH_LIST_EACH_OTHER);
    CHECK(answer.status == SPISOR_OK);
    CHECK(ids_are(answer, "[\"github:1\",\"github:2\"]"));
    CHECK(flag_is(answer, "startFileMalformed", 0));
    release_call(&answer);
}

static void agreement_does_not_depend_on_which_copy_it_starts_from(void) {
    agreement_call answer = agree("github:2", BOTH_LIST_EACH_OTHER);
    CHECK(ids_are(answer, "[\"github:2\",\"github:1\"]"));
    CHECK(primary_is(answer, "github:1"));
    release_call(&answer);
}

static void a_one_sided_claim_is_not_counted(void) {
    agreement_call answer = agree("github:1",
                                  "{\"github:1\":{\"primary\":\"github:1\",\"ids\":[\"github:1\",\"github:2\"]},"
                                  "\"github:2\":{\"primary\":\"github:2\",\"ids\":[\"github:2\"]}}");
    CHECK(answer.status == SPISOR_OK);
    CHECK(ids_are(answer, "[\"github:1\"]"));
    release_call(&answer);
}

static void a_copy_that_lists_us_but_is_not_listed_by_us_is_not_counted(void) {
    agreement_call answer = agree("github:1",
                                  "{\"github:1\":{\"primary\":\"github:1\",\"ids\":[\"github:1\"]},"
                                  "\"github:2\":{\"primary\":\"github:1\",\"ids\":[\"github:1\",\"github:2\"]}}");
    CHECK(ids_are(answer, "[\"github:1\"]"));
    release_call(&answer);
}

static void an_unreachable_copy_is_left_out_and_is_no_refusal(void) {
    agreement_call answer = agree("github:1",
                                  "{\"github:1\":{\"primary\":\"github:1\",\"ids\":[\"github:1\",\"github:2\",\"github:3\"]},"
                                  "\"github:2\":null,"
                                  "\"github:3\":{\"primary\":\"github:1\",\"ids\":[\"github:1\",\"github:3\"]}}");
    CHECK(answer.status == SPISOR_OK);
    CHECK(ids_are(answer, "[\"github:1\",\"github:3\"]"));
    release_call(&answer);
}

static void a_copy_with_no_entry_at_all_is_left_out_too(void) {
    agreement_call answer =
        agree("github:1", "{\"github:1\":{\"primary\":\"github:1\",\"ids\":[\"github:1\",\"github:2\"]}}");
    CHECK(ids_are(answer, "[\"github:1\"]"));
    release_call(&answer);
}

static void no_also_at_file_means_only_the_id_itself(void) {
    agreement_call answer = agree("github:1", "{\"github:1\":null}");
    CHECK(answer.status == SPISOR_OK);
    CHECK(ids_are(answer, "[\"github:1\"]"));
    CHECK(primary_is(answer, "github:1"));
    CHECK(flag_is(answer, "primaryConfirmed", 1));
    CHECK(flag_is(answer, "startFileMalformed", 0));
    release_call(&answer);
}

static void the_primary_is_the_one_both_name(void) {
    agreement_call answer = agree("github:2",
                                  "{\"github:2\":{\"primary\":\"github:1\",\"ids\":[\"github:2\",\"github:1\"]},"
                                  "\"github:1\":{\"primary\":\"github:1\",\"ids\":[\"github:1\",\"github:2\"]}}");
    CHECK(primary_is(answer, "github:1"));
    CHECK(flag_is(answer, "primaryConfirmed", 1));
    release_call(&answer);
}

static void a_primary_that_could_not_be_confirmed_says_so(void) {
    agreement_call answer = agree("github:2",
                                  "{\"github:2\":{\"primary\":\"github:1\",\"ids\":[\"github:2\",\"github:1\"]},"
                                  "\"github:1\":null}");
    CHECK(primary_is(answer, "github:1"));
    CHECK(flag_is(answer, "primaryConfirmed", 0));
    CHECK(ids_are(answer, "[\"github:2\"]"));
    release_call(&answer);
}

static void disagreeing_primaries_are_refused_with_a_fix(void) {
    agreement_call answer = agree("github:1",
                                  "{\"github:1\":{\"primary\":\"github:1\",\"ids\":[\"github:1\",\"github:2\"]},"
                                  "\"github:2\":{\"primary\":\"github:2\",\"ids\":[\"github:1\",\"github:2\"]}}");
    CHECK(expect_refusal(answer, "primaries", "disagreeing primaries"));
    CHECK(answer.refusal.fix != NULL && strstr(answer.refusal.fix, "same primary") != NULL);
    release_call(&answer);
}

static void an_id_outside_the_grammar_is_left_out(void) {
    agreement_call answer = agree("github:1",
                                  "{\"github:1\":{\"primary\":\"github:1\",\"ids\":[\"github:1\",\"github:../x\",\"github:2\"]},"
                                  "\"github:2\":{\"primary\":\"github:1\",\"ids\":[\"github:1\",\"github:2\"]}}");
    CHECK(answer.status == SPISOR_OK);
    CHECK(ids_are(answer, "[\"github:1\"]"));
    release_call(&answer);
}

static void a_start_file_that_could_not_be_read_is_not_a_confirmed_primary(void) {
    agreement_call answer = agree("github:1", "{\"github:1\":false}");
    CHECK(answer.status == SPISOR_OK);
    CHECK(ids_are(answer, "[\"github:1\"]"));
    CHECK(primary_is(answer, "github:1"));
    CHECK(flag_is(answer, "primaryConfirmed", 0));
    CHECK(flag_is(answer, "startFileUnavailable", 1));
    CHECK(flag_is(answer, "startFileMalformed", 0));
    release_call(&answer);
}

static void a_listed_copy_that_could_not_be_read_is_left_out(void) {
    agreement_call answer = agree("github:1",
                                  "{\"github:1\":{\"primary\":\"github:1\",\"ids\":[\"github:1\",\"github:2\"]},"
                                  "\"github:2\":false}");
    CHECK(answer.status == SPISOR_OK);
    CHECK(ids_are(answer, "[\"github:1\"]"));
    CHECK(flag_is(answer, "startFileUnavailable", 0));
    release_call(&answer);
}

static void a_malformed_start_file_is_reported_in_the_answer(void) {
    const char *malformed[] = {"{\"github:1\":{\"primary\":\"nonsense\",\"ids\":[\"github:1\"]}}",
                               "{\"github:1\":{\"primary\":\"github:1\"}}",
                               "{\"github:1\":{\"primary\":\"github:1\",\"ids\":\"github:1\"}}",
                               "{\"github:1\":{\"primary\":\"github:1\",\"ids\":[\"local:folder\"]}}",
                               "{\"github:1\":\"not an also-at file\"}"};
    for (size_t index = 0; index < sizeof malformed / sizeof malformed[0]; index++) {
        agreement_call answer = agree("github:1", malformed[index]);
        CHECK(answer.status == SPISOR_OK);
        CHECK(ids_are(answer, "[\"github:1\"]"));
        CHECK(flag_is(answer, "startFileMalformed", 1));
        release_call(&answer);
    }
}

static void a_malformed_file_on_another_copy_leaves_only_that_copy_out(void) {
    agreement_call answer = agree("github:1",
                                  "{\"github:1\":{\"primary\":\"github:1\",\"ids\":[\"github:1\",\"github:2\",\"github:3\"]},"
                                  "\"github:2\":{\"primary\":7,\"ids\":[\"github:1\",\"github:2\"]},"
                                  "\"github:3\":{\"primary\":\"github:1\",\"ids\":[\"github:1\",\"github:3\"]}}");
    CHECK(answer.status == SPISOR_OK);
    CHECK(ids_are(answer, "[\"github:1\",\"github:3\"]"));
    CHECK(flag_is(answer, "startFileMalformed", 0));
    release_call(&answer);
}

static void a_file_listing_more_than_sixteen_ids_is_malformed(void) {
    char text[1024] = "{\"github:1\":{\"primary\":\"github:1\",\"ids\":[\"github:1\"";
    for (int number = 2; number <= 17; number++) {
        char more[32];
        snprintf(more, sizeof more, ",\"github:%d\"", number);
        strcat(text, more);
    }
    strcat(text, "]}}");
    agreement_call answer = agree("github:1", text);
    CHECK(ids_are(answer, "[\"github:1\"]"));
    CHECK(flag_is(answer, "startFileMalformed", 1));
    release_call(&answer);
}

static void sixteen_ids_are_allowed(void) {
    char text[2048] = "{\"github:1\":{\"primary\":\"github:1\",\"ids\":[\"github:1\"";
    for (int number = 2; number <= 16; number++) {
        char more[32];
        snprintf(more, sizeof more, ",\"github:%d\"", number);
        strcat(text, more);
    }
    strcat(text, "]}}");
    agreement_call answer = agree("github:1", text);
    CHECK(flag_is(answer, "startFileMalformed", 0));
    release_call(&answer);
}

static void a_start_id_that_is_not_a_repo_is_malformed(void) {
    const char *refused[] = {"calendar", "local:folder", "github:1/kernel"};
    for (size_t index = 0; index < sizeof refused / sizeof refused[0]; index++) {
        agreement_call answer = agree(refused[index], "{}");
        CHECK(check_refused(answer.status, SPISOR_MALFORMED, &answer.refusal, "id", refused[index]));
        release_call(&answer);
        answer = also_at_root("http://127.0.0.1:1", refused[index]);
        CHECK(check_refused(answer.status, SPISOR_MALFORMED, &answer.refusal, "id", refused[index]));
        release_call(&answer);
    }
}

static void a_copy_listed_twice_or_without_an_id_is_malformed(void) {
    runtime_identity_copy copies[2] = {{"github:2", 0, NULL}, {"github:2", 1, NULL}};
    runtime_identity_agree_argument argument = SPISOR_INIT(runtime_identity_agree_argument);
    agreement_call call = {SPISOR_OK, SPISOR_INIT(runtime_identity_agreement), SPISOR_INIT(spisor_refusal)};
    argument.id = "github:1";
    argument.copies = copies;
    argument.copy_count = 2;
    call.status = identity()->agree(identity()->context, &argument, &call.answer, &call.refusal);
    CHECK(check_refused(call.status, SPISOR_MALFORMED, &call.refusal, "listed twice", "a copy listed twice"));
    release_call(&call);
    copies[1].id = NULL;
    call.status = identity()->agree(identity()->context, &argument, &call.answer, &call.refusal);
    CHECK(check_refused(call.status, SPISOR_MALFORMED, &call.refusal, "no id", "a copy without an id"));
    release_call(&call);
    argument.copies = NULL;
    call.status = identity()->agree(identity()->context, &argument, &call.answer, &call.refusal);
    CHECK(check_refused(call.status, SPISOR_MALFORMED, &call.refusal, "copy", "a count without copies"));
    release_call(&call);
}

static void reads_the_files_of_the_copies_its_own_file_lists(void) {
    agreement_call answer = also_at("github:1");
    CHECK(answer.status == SPISOR_OK);
    CHECK(ids_are(answer, "[\"github:1\",\"github:2\"]"));
    CHECK(primary_is(answer, "github:1"));
    release_call(&answer);
}

static void leaves_out_a_copy_the_host_cannot_find(void) {
    agreement_call answer = also_at("github:10");
    CHECK(answer.status == SPISOR_OK);
    CHECK(ids_are(answer, "[\"github:10\",\"github:11\"]"));
    release_call(&answer);
}

static void leaves_out_a_copy_that_is_a_one_sided_claim(void) {
    agreement_call answer = also_at("github:20");
    CHECK(ids_are(answer, "[\"github:20\"]"));
    release_call(&answer);
}

static void does_not_follow_ids_beyond_the_set_its_own_file_lists(void) {
    agreement_call answer = also_at("github:30");
    CHECK(ids_are(answer, "[\"github:30\",\"github:31\"]"));
    release_call(&answer);
}

static void answers_only_the_id_when_it_has_no_file(void) {
    agreement_call answer = also_at("github:40");
    CHECK(answer.status == SPISOR_OK);
    CHECK(ids_are(answer, "[\"github:40\"]"));
    CHECK(flag_is(answer, "startFileMalformed", 0));
    release_call(&answer);
}

static void reports_a_malformed_file_of_the_id_itself(void) {
    const char *ids[] = {"github:50", "github:51"};
    for (size_t index = 0; index < 2; index++) {
        agreement_call answer = also_at(ids[index]);
        CHECK(answer.status == SPISOR_OK);
        CHECK(ids_are(answer, index == 0 ? "[\"github:50\"]" : "[\"github:51\"]"));
        CHECK(flag_is(answer, "startFileMalformed", 1));
        release_call(&answer);
    }
}

static void says_so_when_the_start_copy_answers_a_server_error(void) {
    agreement_call answer = also_at("github:80");
    CHECK(answer.status == SPISOR_OK);
    CHECK(ids_are(answer, "[\"github:80\"]"));
    CHECK(flag_is(answer, "primaryConfirmed", 0));
    CHECK(flag_is(answer, "startFileUnavailable", 1));
    release_call(&answer);
}

static void leaves_out_a_listed_copy_that_answers_a_server_error(void) {
    agreement_call answer = also_at("github:81");
    CHECK(answer.status == SPISOR_OK);
    CHECK(ids_are(answer, "[\"github:81\"]"));
    CHECK(flag_is(answer, "primaryConfirmed", 1));
    CHECK(flag_is(answer, "startFileUnavailable", 0));
    release_call(&answer);
}

static void does_not_keep_a_file_when_the_name_changed_hands_during_the_read(void) {
    agreement_call answer = also_at("github:90");
    CHECK(answer.status == SPISOR_OK);
    CHECK(ids_are(answer, "[\"github:90\"]"));
    CHECK(flag_is(answer, "startFileUnavailable", 1));
    CHECK(flag_is(answer, "primaryConfirmed", 0));
    release_call(&answer);
}

static void refuses_when_the_id_itself_cannot_be_resolved(void) {
    agreement_call answer = also_at("github:60");
    CHECK(expect_refusal(answer, "not found", "unresolvable start"));
    release_call(&answer);
}

static void reads_a_gitea_copy_from_its_raw_file_route(void) {
    char api_root[128];
    snprintf(api_root, sizeof api_root, "http://127.0.0.1:%u/api/v1", (unsigned)server_port);
    agreement_call answer = also_at_root(api_root, "gitea@git.example.com:70");
    CHECK(ids_are(answer, "[\"gitea@git.example.com:70\",\"gitea@git.example.com:71\"]"));
    release_call(&answer);
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
    spisor_memory_release(http_fixture_stop(server));
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
    naula_fetch_allow_plain_http(1);
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
    a_start_id_that_is_not_a_repo_is_malformed();
    a_copy_listed_twice_or_without_an_id_is_malformed();
    run_against_the_server();
    return check_summary("identity_also_at_test");
}
