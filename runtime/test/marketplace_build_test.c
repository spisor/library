#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog_call.h"
#include "spisor/allocation.h"
#include "check.h"
#include "fake_forge.h"
#include "marketplace_call.h"
#include "marketplace_build.h"
#include "marketplace_check.h"

#define BUILT "2026-11-10T12:00:00Z"
#define ROOT_RUNTIME "1000000000000000000000000000000000000001"
#define ROOT_WEIVEL "2000000000000000000000000000000000000002"
#define ROOT_OTHER "3000000000000000000000000000000000000003"
#define ROOT_MIXED "5000000000000000000000000000000000000005"
#define ROOT_RESERVED "6000000000000000000000000000000000000006"
#define ROOT_GONE "7000000000000000000000000000000000000007"
#define RUNTIME_ID "git:" ROOT_RUNTIME
#define WEIVEL_ID "git:" ROOT_WEIVEL
#define OTHER_ID "git:" ROOT_OTHER
#define MIXED_ID "git:" ROOT_MIXED
#define RESERVED_ID "git:" ROOT_RESERVED
#define GONE_ID "git:" ROOT_GONE
#define ENTRY(id, place) "{\"id\":\"" id "\",\"at\":[\"" place "\"]"
#define LIST(entries) "{\"format\":2,\"entries\":[" entries "]}"
#define TOKEN "tok-1234567890abcdef"
#define KEPT_ENTRY                                                                                                     \
    "{\"id\":\"" GONE_ID "\",\"at\":[\"forge:70\"],\"name\":\"kept\",\"fullName\":\"spisor/kept\",\"description\":\"Old.\"}"
#define PREVIOUS_COPY(entries) "{\"format\":2,\"built\":\"2026-11-01T12:00:00Z\",\"entries\":[" entries "]}"

static fake_repo *add(uint64_t number, const char *account, const char *name, const char *root) {
    fake_repo *repo = fake_forge_add(number, account, name, NULL);
    char homes[128];
    snprintf(homes, sizeof homes, "{\"provider\":\"forge\",\"account\":\"%s\"}", account);
    repo->file = fake_forge_file(name, homes);
    repo->root = root;
    return repo;
}

/** The repos most cases list: runtime at 1, weivel at 2, a repo named after runtime at 4, two awkward names. */
static void start_forge(void) {
    fake_forge_reset();
    add(1, "spisor", "runtime", ROOT_RUNTIME);
    add(2, "spisor", "weivel", ROOT_WEIVEL);
    add(4, "other", "runtime", ROOT_OTHER);
    add(5, "spisor", "My_Runtime.JS", ROOT_MIXED);
    add(6, "spisor", "package", ROOT_RESERVED);
}

static catalog_answer build_with_options(const char *list_json, const char *token, const char *token_host,
                                         const char *previous_json, const char *built) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "list", parsed(list_json));
    cJSON_AddStringToObject(argument, "built", built);
    if (token != NULL) {
        cJSON_AddStringToObject(argument, "token", token);
    }
    if (token_host != NULL) {
        cJSON_AddStringToObject(argument, "tokenHost", token_host);
    }
    if (previous_json != NULL) {
        cJSON_AddItemToObject(argument, "previous", parsed(previous_json));
    }
    return marketplace_call(MARKETPLACE_BUILD, argument);
}

static catalog_answer build(const char *list_json) {
    return build_with_options(list_json, NULL, NULL, NULL, BUILT);
}

static const cJSON *entry_at(const catalog_answer answer, int index) {
    return cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(value_at(answer.tree, "copy"), "entries"), index);
}

static int entry_member_is(const catalog_answer answer, int index, const char *member, const char *expected) {
    return check_text(text_at(entry_at(answer, index), member), expected, member);
}

static int entry_count(const catalog_answer answer) {
    return cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(value_at(answer.tree, "copy"), "entries"));
}

static int contains(const char *text, const char *fragment) {
    return text != NULL && strstr(text, fragment) != NULL;
}

static void builds_a_copy_with_current_names_and_the_places(void) {
    start_forge();
    catalog_answer answer = build("{\"format\":2,\"title\":\"Spisor\",\"entries\":["
                                  ENTRY(RUNTIME_ID, "forge:1") ",\"description\":\"The runtime.\",\"categories\":[\"core\"]},"
                                  ENTRY(WEIVEL_ID, "forge:2") "}]}");
    CHECK(answer.status == SPISOR_OK);
    const cJSON *copy = value_at(answer.tree, "copy");
    CHECK(cJSON_GetObjectItemCaseSensitive(copy, "format")->valueint == 2);
    check_text(text_at(copy, "title"), "Spisor", "title");
    check_text(text_at(copy, "built"), BUILT, "built");
    CHECK(entry_count(answer) == 2);
    entry_member_is(answer, 0, "id", RUNTIME_ID);
    entry_member_is(answer, 0, "name", "runtime");
    entry_member_is(answer, 0, "fullName", "spisor/runtime");
    entry_member_is(answer, 0, "description", "The runtime.");
    check_text(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(entry_at(answer, 0), "at"), 0)->valuestring,
               "forge:1", "the places are carried");
    CHECK(!cJSON_HasObjectItem(entry_at(answer, 0), "lastKnown"));
    entry_member_is(answer, 1, "name", "weivel");
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(answer.tree, "skipped")) == 0);
    cJSON_Delete(answer.tree);
}

static void a_renamed_repo_gets_its_new_name_with_no_change_to_the_list(void) {
    start_forge();
    fake_repo *runtime = add(1, "spisor", "runtime-next", ROOT_RUNTIME);
    (void)runtime;
    catalog_answer answer = build(LIST(ENTRY(RUNTIME_ID, "forge:1") "}"));
    CHECK(answer.status == SPISOR_OK);
    entry_member_is(answer, 0, "name", "runtime-next");
    entry_member_is(answer, 0, "fullName", "spisor/runtime-next");
    cJSON_Delete(answer.tree);
}

static void names_clash_unless_an_alias_settles_it(void) {
    start_forge();
    catalog_answer clash = build(LIST(ENTRY(RUNTIME_ID, "forge:1") "}," ENTRY(OTHER_ID, "forge:4") "}"));
    CHECK(expect_refusal(clash, "both named runtime", "two entries of one name"));
    CHECK(contains(text_at(clash.tree, "message"), RUNTIME_ID) && contains(text_at(clash.tree, "message"), OTHER_ID));
    cJSON_Delete(clash.tree);
    catalog_answer settled =
        build(LIST(ENTRY(RUNTIME_ID, "forge:1") "}," ENTRY(OTHER_ID, "forge:4") ",\"alias\":\"other-runtime\"}"));
    CHECK(settled.status == SPISOR_OK);
    entry_member_is(settled, 1, "name", "other-runtime");
    cJSON_Delete(settled.tree);
    catalog_answer after = build(LIST(ENTRY(RUNTIME_ID, "forge:1") ",\"alias\":\"weivel\"}," ENTRY(WEIVEL_ID, "forge:2") "}"));
    CHECK(expect_refusal(after, "both named weivel", "a clash once aliases apply"));
    cJSON_Delete(after.tree);
}

static void a_duplicate_a_local_or_an_old_id_is_refused_before_anything_resolves(void) {
    start_forge();
    catalog_answer duplicate = build(LIST(ENTRY(RUNTIME_ID, "forge:1") "}," ENTRY(RUNTIME_ID, "forge:1") "}"));
    CHECK(expect_refusal(duplicate, "twice", "a duplicate"));
    cJSON_Delete(duplicate.tree);
    catalog_answer local = build(LIST(ENTRY(RUNTIME_ID, "forge:1") "}," ENTRY("local:tool", "forge:1") "}"));
    CHECK(expect_refusal(local, "local:tool", "a local id"));
    cJSON_Delete(local.tree);
    catalog_answer old = build(LIST(ENTRY("forge:1", "forge:1") "}"));
    CHECK(expect_refusal(old, "list", "an id of the old grammar"));
    cJSON_Delete(old.tree);
    CHECK(fake_forge_calls() == 0);
}

static void a_folder_id_builds_with_the_last_segment_as_its_name(void) {
    start_forge();
    catalog_answer answer =
        build(LIST(ENTRY(RUNTIME_ID "/kernel", "forge:1") "}," ENTRY(RUNTIME_ID "/tools/lint", "forge:1") "}"));
    CHECK(answer.status == SPISOR_OK);
    entry_member_is(answer, 0, "name", "kernel");
    entry_member_is(answer, 0, "fullName", "spisor/runtime");
    entry_member_is(answer, 1, "name", "lint");
    cJSON_Delete(answer.tree);
    catalog_answer clash = build(LIST(ENTRY(RUNTIME_ID "/a/kernel", "forge:1") "}," ENTRY(RUNTIME_ID "/b/kernel", "forge:1") "}"));
    CHECK(expect_refusal(clash, "both named kernel", "two folders with one last segment"));
    cJSON_Delete(clash.tree);
}

static void a_name_is_derived_or_the_entry_is_skipped(void) {
    start_forge();
    catalog_answer answer = build(LIST(ENTRY(MIXED_ID, "forge:5") "}," ENTRY(RESERVED_ID, "forge:6") "}," ENTRY(RUNTIME_ID, "forge:1") "}"));
    CHECK(answer.status == SPISOR_OK);
    CHECK(entry_count(answer) == 2);
    entry_member_is(answer, 0, "name", "my-runtime.js");
    entry_member_is(answer, 1, "name", "runtime");
    const cJSON *skipped = cJSON_GetObjectItemCaseSensitive(answer.tree, "skipped");
    CHECK(cJSON_GetArraySize(skipped) == 1);
    check_text(text_at(cJSON_GetArrayItem(skipped, 0), "id"), RESERVED_ID, "skipped id");
    CHECK(contains(text_at(cJSON_GetArrayItem(skipped, 0), "reason"), "cannot be made an import name"));
    cJSON_Delete(answer.tree);
    catalog_answer aliased = build(LIST(ENTRY(RESERVED_ID, "forge:6") ",\"alias\":\"packaged\"}"));
    CHECK(aliased.status == SPISOR_OK);
    entry_member_is(aliased, 0, "name", "packaged");
    cJSON_Delete(aliased.tree);
}

static void a_list_or_time_that_breaks_its_shape_is_refused(void) {
    start_forge();
    catalog_answer no_at = build(LIST("{\"id\":\"" RUNTIME_ID "\"}"));
    CHECK(expect_refusal(no_at, "at", "an entry without places"));
    cJSON_Delete(no_at.tree);
    catalog_answer bad_place = build(LIST(ENTRY(RUNTIME_ID, "forge:0") "}"));
    CHECK(expect_refusal(bad_place, "at", "a place that is none"));
    cJSON_Delete(bad_place.tree);
    catalog_answer built = build_with_options(LIST(ENTRY(RUNTIME_ID, "forge:1") "}"), NULL, NULL, NULL, "yesterday");
    CHECK(expect_refusal(built, "RFC 3339", "a built that is no time"));
    cJSON_Delete(built.tree);
    catalog_answer empty = build("{\"format\":2,\"entries\":[]}");
    CHECK(empty.status == SPISOR_OK && entry_count(empty) == 0);
    cJSON_Delete(empty.tree);
}

static void an_entry_naula_does_not_confirm_is_skipped_or_carried_over(void) {
    start_forge();
    catalog_answer skipped = build(LIST(ENTRY(RUNTIME_ID, "forge:1") "}," ENTRY(GONE_ID, "forge:70") "}"));
    CHECK(skipped.status == SPISOR_OK && entry_count(skipped) == 1);
    const cJSON *row = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(skipped.tree, "skipped"), 0);
    check_text(text_at(row, "id"), GONE_ID, "skipped id");
    CHECK(contains(text_at(row, "reason"), "cannot be resolved"));
    cJSON_Delete(skipped.tree);
    catalog_answer kept = build_with_options(LIST(ENTRY(RUNTIME_ID, "forge:1") "}," ENTRY(GONE_ID, "forge:70") "}"), NULL,
                                             NULL, PREVIOUS_COPY(KEPT_ENTRY), BUILT);
    CHECK(kept.status == SPISOR_OK && entry_count(kept) == 2);
    entry_member_is(kept, 1, "name", "kept");
    entry_member_is(kept, 1, "description", "Old.");
    cJSON_Delete(kept.tree);
    catalog_answer only = build(LIST(ENTRY(GONE_ID, "forge:70") "}"));
    CHECK(expect_refusal(only, "no listed repository could be built", "a list of only a gone repo"));
    cJSON_Delete(only.tree);
    catalog_answer not_a_copy =
        build_with_options(LIST(ENTRY(RUNTIME_ID, "forge:1") "}"), NULL, NULL, "{\"format\":2,\"entries\":[]}", BUILT);
    CHECK(expect_refusal(not_a_copy, "previous is not a marketplace copy", "a previous that is no copy"));
    cJSON_Delete(not_a_copy.tree);
}

static void a_list_every_host_rate_limits_says_so(void) {
    start_forge();
    fake_repo *limited = add(80, "spisor", "limited", ROOT_GONE);
    limited->refused_with = "rate_limited";
    catalog_answer answer = build(LIST(ENTRY(GONE_ID, "forge:80") "}"));
    CHECK(expect_refusal(answer, "no listed repository could be built", "a rate limited list"));
    CHECK(contains(text_at(answer.tree, "fix"), "rate limiting"));
    cJSON_Delete(answer.tree);
}

static void a_token_goes_only_to_its_host_and_into_neither_the_copy_nor_a_refusal(void) {
    fake_forge_reset();
    fake_forge_add(1, "spisor", "runtime",
                   fake_forge_file("runtime", "{\"provider\":\"forge\",\"host\":\"git.example.com\",\"account\":\"spisor\"}"))
        ->root = ROOT_RUNTIME;
    catalog_answer answer = build_with_options(LIST(ENTRY(RUNTIME_ID, "forge@git.example.com:1") "}"), TOKEN,
                                               "git.example.com", NULL, BUILT);
    CHECK(answer.status == SPISOR_OK && entry_count(answer) == 1);
    CHECK(fake_forge_calls_with_token(TOKEN) > 0);
    char *printed = cJSON_PrintUnformatted(answer.tree);
    CHECK(!contains(printed, TOKEN));
    cJSON_free(printed);
    cJSON_Delete(answer.tree);
    start_forge();
    catalog_answer elsewhere = build_with_options(LIST(ENTRY(RUNTIME_ID, "forge@other.example:1") "}"), TOKEN,
                                                  "git.example.com", NULL, BUILT);
    CHECK(expect_refusal(elsewhere, "the one host the token is sent to", "a place on another host"));
    CHECK(!contains(text_at(elsewhere.tree, "message"), TOKEN) && fake_forge_calls() == 0);
    cJSON_Delete(elsewhere.tree);
    catalog_answer bare = build_with_options(LIST(ENTRY(RUNTIME_ID, "forge:1") "}"), TOKEN, "git.example.com", NULL, BUILT);
    CHECK(expect_refusal(bare, "the one host the token is sent to", "a place naming no host"));
    cJSON_Delete(bare.tree);
    catalog_answer unusable =
        build_with_options(LIST(ENTRY(RUNTIME_ID, "forge@git.example.com:1") "}"), "two words", "git.example.com", NULL, BUILT);
    CHECK(expect_refusal(unusable, "token is not usable", "an unusable token"));
    CHECK(!contains(text_at(unusable.tree, "message"), "two words") && fake_forge_calls() == 0);
    cJSON_Delete(unusable.tree);
}

static char *list_of_repositories(size_t count) {
    size_t size = count * 80 + 64;
    char *text = spisor_memory_allocate(size);
    size_t used = (size_t)snprintf(text, size, "{\"format\":2,\"entries\":[");
    for (size_t index = 1; index <= count; index++) {
        used += (size_t)snprintf(text + used, size - used, "%s" ENTRY(RUNTIME_ID, "forge:1") "}", index == 1 ? "" : ",");
    }
    snprintf(text + used, size - used, "]}");
    return text;
}

static void a_list_over_the_limit_is_refused_before_anything_resolves(void) {
    start_forge();
    char *list = list_of_repositories(RUNTIME_MARKETPLACE_MAXIMUM_ENTRIES + 1);
    catalog_answer answer = build(list);
    CHECK(expect_refusal(answer, "list", "a list over the limit"));
    CHECK(fake_forge_calls() == 0);
    cJSON_Delete(answer.tree);
    spisor_memory_release(list);
}

static void the_list_checker_follows_the_schema(void) {
    char problem[256];
    cJSON *minimal = parsed(LIST(ENTRY(RUNTIME_ID, "forge:1") "}"));
    CHECK(runtime_marketplace_check_list(minimal, problem, sizeof problem) == 1);
    cJSON_Delete(minimal);
    const char *refused[] = {LIST("{\"alias\":\"core\",\"at\":[\"forge:1\"]}"),
                             LIST(ENTRY("../" RUNTIME_ID, "forge:1") "}"),
                             LIST(ENTRY(RUNTIME_ID, "forge:1") ",\"name\":\"runtime\"}"),
                             LIST(ENTRY(RUNTIME_ID, "forge:1") ",\"categories\":[\"a\",\"a\"]}"),
                             LIST(ENTRY(RUNTIME_ID, "forge:1") ",\"alias\":\"package\"}"),
                             LIST("{\"id\":\"" RUNTIME_ID "\",\"at\":[]}"),
                             LIST("{\"id\":\"" RUNTIME_ID "\",\"at\":\"forge:1\"}"),
                             "{\"format\":2,\"entries\":[],\"extra\":true}",
                             "{\"entries\":[]}"};
    for (size_t index = 0; index < sizeof refused / sizeof refused[0]; index++) {
        cJSON *list = parsed(refused[index]);
        CHECK(runtime_marketplace_check_list(list, problem, sizeof problem) == 0 && problem[0] != 0);
        cJSON_Delete(list);
    }
}

static void the_copy_checker_follows_the_schema(void) {
    char problem[256];
    cJSON *copy = parsed("{\"format\":2,\"built\":\"" BUILT "\",\"entries\":[" ENTRY(RUNTIME_ID, "forge:1")
                         ",\"name\":\"runtime\",\"fullName\":\"spisor/runtime\"}]}");
    CHECK(runtime_marketplace_check_copy(copy, problem, sizeof problem) == 1);
    cJSON_Delete(copy);
    const char *refused[] = {
        "{\"format\":2,\"entries\":[]}",
        "{\"format\":2,\"built\":\"2026-13-10T12:00:00Z\",\"entries\":[]}",
        "{\"format\":2,\"built\":\"" BUILT "\",\"entries\":[" ENTRY(RUNTIME_ID, "forge:1") ",\"name\":\"runtime\"}]}",
        "{\"format\":2,\"built\":\"" BUILT "\",\"entries\":[" ENTRY(RUNTIME_ID, "forge:1")
        ",\"name\":\"runtime\",\"fullName\":\"runtime\"}]}",
        "{\"format\":2,\"built\":\"" BUILT "\",\"entries\":[" ENTRY(RUNTIME_ID, "forge:1")
        ",\"name\":\"Runtime\",\"fullName\":\"a/b\"}]}",
        "{\"format\":2,\"built\":\"" BUILT "\",\"entries\":[" ENTRY(RUNTIME_ID, "forge:1")
        ",\"name\":\"runtime\",\"fullName\":\"a/b\",\"lastKnown\":\"a/b\"}]}"};
    for (size_t index = 0; index < sizeof refused / sizeof refused[0]; index++) {
        cJSON *bad = parsed(refused[index]);
        CHECK(runtime_marketplace_check_copy(bad, problem, sizeof problem) == 0 && problem[0] != 0);
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

int main(void) {
    fake_forge_install();
    builds_a_copy_with_current_names_and_the_places();
    a_renamed_repo_gets_its_new_name_with_no_change_to_the_list();
    names_clash_unless_an_alias_settles_it();
    a_duplicate_a_local_or_an_old_id_is_refused_before_anything_resolves();
    a_folder_id_builds_with_the_last_segment_as_its_name();
    a_name_is_derived_or_the_entry_is_skipped();
    a_list_or_time_that_breaks_its_shape_is_refused();
    an_entry_naula_does_not_confirm_is_skipped_or_carried_over();
    a_list_every_host_rate_limits_says_so();
    a_token_goes_only_to_its_host_and_into_neither_the_copy_nor_a_refusal();
    a_list_over_the_limit_is_refused_before_anything_resolves();
    the_list_checker_follows_the_schema();
    the_copy_checker_follows_the_schema();
    the_time_and_full_name_checks_follow_their_patterns();
    the_checkers_hold_what_the_schemas_say();
    return check_summary("marketplace_build_test");
}
