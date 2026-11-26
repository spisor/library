#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <string.h>

#include "check.h"
#include "definition_fixture.h"
#include "fake_forge.h"
#include "identity_naula.h"
#include "runtime/identity.h"
#include "service_scratch.h"

#define HEX64 "5e3d9c4b2a1f0e9d8c7b6a5f4e3d2c1b0a9f8e7d6c5b4a3f2e1d0c9b8a7f6e5d"

static const runtime_identity_definition *identity(void) {
    return runtime_identity_definition_of();
}

/** Parses `id` into `parts`, which the caller releases. */
static spisor_status parse_into(const char *id, runtime_identity_parts *parts) {
    runtime_identity_id_argument argument = SPISOR_INIT(runtime_identity_id_argument);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    argument.id = id;
    status = identity()->parse(identity()->context, &argument, parts, &refusal);
    identity()->release_refusal(identity()->context, &refusal);
    return status;
}

static int is_valid(const char *id) {
    runtime_identity_parts parts = SPISOR_INIT(runtime_identity_parts);
    spisor_status status = parse_into(id, &parts);
    CHECK(status == SPISOR_OK || status == SPISOR_NONE);
    CHECK((status == SPISOR_OK) == (parts.kind != NULL));
    identity()->release_parts(identity()->context, &parts);
    return status == SPISOR_OK;
}

static void the_grammar_is_naulas(void) {
    static const char *const valid[] = {FAKE_ID, "git:" HEX64, FAKE_ID "/kernel", FAKE_ID "/a/b.c", "local:plugin-stub",
                                        "local:a:b"};
    static const char *const invalid[] = {"forge:1434954468", "gitea@git.example.com:123", "git:DD92836C",
                                          "git:dd92836", FAKE_ID "/..", FAKE_ID "/A", "local:", "calendar", ""};
    for (size_t index = 0; index < sizeof valid / sizeof valid[0]; index++) {
        if (!is_valid(valid[index])) {
            fprintf(stderr, "\"%s\" should be valid\n", valid[index]);
            check_failures++;
        }
    }
    for (size_t index = 0; index < sizeof invalid / sizeof invalid[0]; index++) {
        if (is_valid(invalid[index])) {
            fprintf(stderr, "\"%s\" should be invalid\n", invalid[index]);
            check_failures++;
        }
    }
}

static void a_git_id_parses_to_its_commit_and_path(void) {
    runtime_identity_parts parts = SPISOR_INIT(runtime_identity_parts);
    CHECK(parse_into(FAKE_ID "/a/b.c", &parts) == SPISOR_OK);
    check_text(parts.kind, "git", "kind");
    check_text(parts.commit, FAKE_ROOT, "commit");
    check_text(parts.path, "a/b.c", "path");
    CHECK(parts.name == NULL);
    identity()->release_parts(identity()->context, &parts);
    CHECK(parts.storage == NULL && parts.kind == NULL);
    CHECK(parse_into(FAKE_ID, &parts) == SPISOR_OK);
    CHECK(parts.path == NULL);
    identity()->release_parts(identity()->context, &parts);
}

static void a_local_id_has_a_name_and_no_commit(void) {
    runtime_identity_parts parts = SPISOR_INIT(runtime_identity_parts);
    CHECK(parse_into("local:x", &parts) == SPISOR_OK);
    check_text(parts.kind, "local", "kind");
    check_text(parts.name, "x", "local name");
    CHECK(parts.commit == NULL && parts.path == NULL);
    identity()->release_parts(identity()->context, &parts);
}

static void an_invalid_id_answers_none_and_no_parts(void) {
    runtime_identity_parts parts = SPISOR_INIT(runtime_identity_parts);
    CHECK(parse_into("forge:1", &parts) == SPISOR_NONE);
    CHECK(parts.kind == NULL && parts.commit == NULL && parts.name == NULL && parts.path == NULL);
    CHECK(parts.storage == NULL);
}

static void a_parse_without_an_id_is_malformed(void) {
    runtime_identity_parts parts = SPISOR_INIT(runtime_identity_parts);
    runtime_identity_id_argument argument = SPISOR_INIT(runtime_identity_id_argument);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    check_refused(identity()->parse(identity()->context, &argument, &parts, &refusal), SPISOR_MALFORMED, &refusal,
                  "no id", "parse without an id");
    identity()->release_refusal(identity()->context, &refusal);
}

typedef struct folder_call {
    spisor_status status;
    runtime_identity_folder_answer answer;
    spisor_refusal refusal;
} folder_call;

static folder_call folder_of(const char *id) {
    runtime_identity_id_argument argument = SPISOR_INIT(runtime_identity_id_argument);
    folder_call call = {SPISOR_OK, SPISOR_INIT(runtime_identity_folder_answer), SPISOR_INIT(spisor_refusal)};
    argument.id = id;
    call.status = identity()->folder(identity()->context, &argument, &call.answer, &call.refusal);
    return call;
}

static void release_folder_call(folder_call *call) {
    identity()->release_folder_answer(identity()->context, &call->answer);
    identity()->release_refusal(identity()->context, &call->refusal);
}

static void expect_folder(const char *id, const char *expected) {
    folder_call call = folder_of(id);
    CHECK(call.status == SPISOR_OK);
    check_text(call.answer.folder, expected, id);
    release_folder_call(&call);
}

static void expect_no_folder(const char *id, spisor_status expected) {
    folder_call call = folder_of(id);
    if (!check_refused(call.status, expected, &call.refusal, NULL, id == NULL ? "(none)" : id) ||
        call.answer.folder != NULL) {
        fprintf(stderr, "%s should have no folder\n", id == NULL ? "(none)" : id);
        check_failures++;
    }
    release_folder_call(&call);
}

static void a_folder_swaps_the_colon_for_a_plus_and_the_slash_for_an_equals_sign(void) {
    expect_folder(FAKE_ID "/kernel", "git+" FAKE_ROOT "=kernel");
    expect_folder("local:plugin-stub", "local+plugin-stub");
    expect_folder("local:a/b", "local+a=b");
}

static void a_folder_no_portable_name_or_one_id_alone_can_carry_is_refused(void) {
    const char *const refused[] = {"local:a=b", "local:a+b", "local:a:b", "local:Foo", "local:a\\b", "local:a?"};
    for (size_t index = 0; index < sizeof refused / sizeof refused[0]; index++) {
        expect_no_folder(refused[index], SPISOR_REFUSED);
    }
    expect_no_folder(FAKE_ID "/a.", SPISOR_REFUSED);
    expect_no_folder("forge:12", SPISOR_MALFORMED);
    expect_no_folder("calendar", SPISOR_MALFORMED);
    expect_no_folder(NULL, SPISOR_MALFORMED);
}

typedef struct resolve_call {
    spisor_status status;
    runtime_identity_resolution answer;
    spisor_refusal refusal;
} resolve_call;

static char cache[1024];

/** Resolves `id` from the places of `at`, a NULL-terminated list, with the accepted places, through `cache`. */
static resolve_call resolve_with(const char *id, const char *const *at, const char *accept_primary,
                                 const char *accept_number) {
    runtime_identity_resolve_argument argument = SPISOR_INIT(runtime_identity_resolve_argument);
    resolve_call call = {SPISOR_OK, SPISOR_INIT(runtime_identity_resolution), SPISOR_INIT(spisor_refusal)};
    size_t count = 0;
    while (at != NULL && at[count] != NULL) {
        count++;
    }
    argument.id = id;
    argument.at = at;
    argument.at_count = count;
    argument.cache = cache[0] == '\0' ? NULL : cache;
    argument.accept_primary = accept_primary;
    argument.accept_number = accept_number;
    call.status = identity()->resolve(identity()->context, &argument, &call.answer, &call.refusal);
    return call;
}

static void release_resolve_call(resolve_call *call) {
    identity()->release_resolution(identity()->context, &call->answer);
    identity()->release_refusal(identity()->context, &call->refusal);
}

static int resolved(const resolve_call *call, const char *label) {
    int matched = call->status == SPISOR_OK && !call->answer.needs_user_acceptance &&
                  !call->answer.needs_number_acceptance;
    if (!matched) {
        fprintf(stderr, "%s: expected a resolution, got status %d and \"%s\"\n", label, (int)call->status,
                call->refusal.detail == NULL ? "(none)" : call->refusal.detail);
        check_failures++;
    }
    return matched;
}

static void start_forge(const char *label) {
    fake_forge_reset();
    scratch_in(cache, sizeof cache, "identity", label);
}

static void a_repo_resolves_to_the_homes_that_list_each_other(void) {
    start_forge("both-ways");
    const char *file = fake_forge_file("lib", FAKE_HOME("alpha") "," FAKE_HOME("bee"));
    fake_forge_add(10, "alpha", "lib", file);
    fake_forge_add(20, "bee", "lib", file);
    const char *at[] = {"forge:10", NULL};
    resolve_call call = resolve_with(FAKE_ID, at, NULL, NULL);
    if (resolved(&call, "both ways")) {
        check_text(call.answer.state, "confirmed", "state");
        CHECK(call.answer.home_count == 2);
        check_text(call.answer.homes[0], "forge:10", "first home");
        check_text(call.answer.homes[1], "forge:20", "second home");
        check_text(call.answer.primary, "forge:10", "primary");
        check_text(call.answer.full_name, "alpha/lib", "full name");
        check_text(call.answer.name, "lib", "name");
        check_text(call.answer.default_branch, "main", "default branch");
    }
    release_resolve_call(&call);
}

static void a_fork_is_refused_as_a_copy(void) {
    start_forge("fork");
    fake_forge_add(10, "alpha", "lib", fake_forge_file("lib", FAKE_HOME("alpha")));
    fake_repo *fork = fake_forge_add(11, "mallory", "lib", fake_forge_file("lib", FAKE_HOME("mallory") "," FAKE_HOME("alpha")));
    fork->fork_parent = 10;
    const char *at[] = {"forge:11", NULL};
    resolve_call call = resolve_with(FAKE_ID, at, NULL, NULL);
    check_refused(call.status, SPISOR_REFUSED, &call.refusal, "copy-of", "a fork");
    CHECK(call.answer.home_count == 0);
    release_resolve_call(&call);
}

static void an_unknown_provider_is_refused_with_the_plugin_fix(void) {
    start_forge("unknown-provider");
    const char *at[] = {"nowhere:7", NULL};
    resolve_call call = resolve_with(FAKE_ID, at, NULL, NULL);
    CHECK(call.status == SPISOR_REFUSED && call.refusal.fix != NULL &&
          strstr(call.refusal.fix, "install a plugin that provides a resolver") != NULL);
    CHECK(call.refusal.detail != NULL && strstr(call.refusal.detail, "no provider is installed for nowhere") != NULL);
    release_resolve_call(&call);
}

/** alpha lists itself first; then bee does, and alpha follows bee, so bee becomes the primary. */
static void make_bee_the_primary(void) {
    const char *moved = fake_forge_file("lib", FAKE_HOME("bee") "," FAKE_HOME("alpha"));
    fake_forge_reset();
    fake_forge_add(10, "alpha", "lib", moved);
    fake_forge_add(20, "bee", "lib", moved);
}

static void a_changed_primary_needs_the_user_until_accepted(void) {
    start_forge("primary");
    const char *file = fake_forge_file("lib", FAKE_HOME("alpha") "," FAKE_HOME("bee"));
    fake_forge_add(10, "alpha", "lib", file);
    fake_forge_add(20, "bee", "lib", file);
    const char *at[] = {"forge:10", NULL};
    resolve_call first = resolve_with(FAKE_ID, at, NULL, NULL);
    CHECK(resolved(&first, "before the move"));
    release_resolve_call(&first);
    make_bee_the_primary();
    resolve_call asked = resolve_with(FAKE_ID, at, NULL, NULL);
    CHECK(asked.status == SPISOR_OK && asked.answer.needs_user_acceptance == 1);
    check_text(asked.answer.recorded_primary, "forge:10", "recorded primary");
    check_text(asked.answer.primary, "forge:20", "new primary");
    CHECK(asked.answer.home_count == 0 && asked.answer.state == NULL);
    release_resolve_call(&asked);
    resolve_call wrong = resolve_with(FAKE_ID, at, "forge:99", NULL);
    CHECK(wrong.status == SPISOR_OK && wrong.answer.needs_user_acceptance == 1);
    release_resolve_call(&wrong);
    resolve_call accepted = resolve_with(FAKE_ID, at, "forge:20", NULL);
    if (resolved(&accepted, "accepted primary")) {
        check_text(accepted.answer.primary, "forge:20", "accepted primary");
    }
    release_resolve_call(&accepted);
    resolve_call again = resolve_with(FAKE_ID, at, NULL, NULL);
    CHECK(resolved(&again, "after acceptance"));
    release_resolve_call(&again);
}

/**
 * One consumer confirmed bee at 20; bee is deleted and re-registered at 21, and another consumer confirmed it there.
 * A third consumer whose at names both of their places reads both records, which disagree on bee's number.
 */
static void a_disagreeing_lineage(const char *label) {
    start_forge(label);
    const char *file = fake_forge_file("lib", FAKE_HOME("alpha") "," FAKE_HOME("bee") "," FAKE_HOME("gamma"));
    fake_forge_add(10, "alpha", "lib", file);
    fake_repo *bee = fake_forge_add(20, "bee", "lib", file);
    fake_forge_add(30, "gamma", "lib", file);
    const char *first_at[] = {"forge:10", NULL};
    resolve_call first = resolve_with(FAKE_ID, first_at, NULL, NULL);
    CHECK(resolved(&first, "the victim"));
    release_resolve_call(&first);
    bee->gone = 1;
    fake_forge_add(21, "bee", "lib", file);
    const char *second_at[] = {"forge:21", "forge:30", NULL};
    resolve_call second = resolve_with(FAKE_ID, second_at, "forge:10", NULL);
    release_resolve_call(&second);
}

static void a_disputed_number_needs_the_user_until_accepted(void) {
    const char *at[] = {"forge:10", "forge:30", NULL};
    a_disagreeing_lineage("number");
    resolve_call asked = resolve_with(FAKE_ID, at, NULL, NULL);
    CHECK(asked.status == SPISOR_OK && asked.answer.needs_number_acceptance == 1 && asked.answer.dispute_count == 1);
    if (asked.answer.dispute_count == 1) {
        check_text(asked.answer.disputes[0].account, "forge:bee", "disputed account");
        check_text(asked.answer.disputes[0].recorded_number, "forge:20", "recorded number");
        check_text(asked.answer.disputes[0].offered_number, "forge:21", "offered number");
        check_text(asked.answer.disputes[0].accept, "forge:bee=21", "how to accept it");
    }
    CHECK(asked.answer.home_count == 0 && asked.answer.needs_user_acceptance == 0);
    release_resolve_call(&asked);
    resolve_call wrong = resolve_with(FAKE_ID, at, NULL, "forge:gamma=21,forge:bee=22");
    CHECK(wrong.status == SPISOR_OK && wrong.answer.needs_number_acceptance == 1);
    release_resolve_call(&wrong);
    resolve_call accepted = resolve_with(FAKE_ID, at, NULL, "forge:gamma=30,forge:bee=21");
    if (resolved(&accepted, "accepted number")) {
        int has_bee = 0;
        for (size_t index = 0; index < accepted.answer.home_count; index++) {
            has_bee |= strcmp(accepted.answer.homes[index], "forge:21") == 0;
        }
        CHECK(has_bee);
    }
    release_resolve_call(&accepted);
}

static naula_refusal refusal_of(const char *code, const char *fix) {
    naula_refusal refusal;
    memset(&refusal, 0, sizeof refusal);
    refusal.size = sizeof refusal;
    snprintf(refusal.code, sizeof refusal.code, "%s", code);
    refusal.fix = fix;
    return refusal;
}

static void a_refusal_that_asks_nothing_wins_over_a_declined_acceptance(void) {
    runtime_identity_acceptance acceptance;
    memset(&acceptance, 0, sizeof acceptance);
    acceptance.dispute_count = 1;
    naula_refusal for_the_number = refusal_of("number-disputed", NULL);
    naula_refusal fix_naming_the_callback = refusal_of("number-changed", "accept the new number through accept_number");
    naula_refusal copy = refusal_of("copy-of", "depend on the original");
    naula_refusal none = refusal_of("", NULL);
    CHECK(runtime_identity_needs_the_user(NAULA_OK, &none, &acceptance));
    CHECK(runtime_identity_needs_the_user(NAULA_REFUSED, &for_the_number, &acceptance));
    CHECK(!runtime_identity_needs_the_user(NAULA_REFUSED, &fix_naming_the_callback, &acceptance));
    CHECK(!runtime_identity_needs_the_user(NAULA_REFUSED, &copy, &acceptance));
    CHECK(!runtime_identity_needs_the_user(NAULA_UNREACHABLE, &copy, &acceptance));
    acceptance.dispute_count = 0;
    acceptance.primary_needed = 1;
    naula_refusal primary = refusal_of("primary-changed", "accept the new primary");
    CHECK(runtime_identity_needs_the_user(NAULA_REFUSED, &primary, &acceptance));
    CHECK(!runtime_identity_needs_the_user(NAULA_REFUSED, &copy, &acceptance));
}

static void a_resolve_without_what_it_needs_is_malformed(void) {
    start_forge("malformed");
    const char *at[] = {"forge:10", NULL};
    const char *bad_at[] = {"forge:zero", NULL};
    resolve_call call = resolve_with(FAKE_ID "/kernel", at, NULL, NULL);
    check_refused(call.status, SPISOR_MALFORMED, &call.refusal, "not the id of a repo", "an id with a folder");
    release_resolve_call(&call);
    call = resolve_with("forge:10", at, NULL, NULL);
    check_refused(call.status, SPISOR_MALFORMED, &call.refusal, "not the id of a repo", "a forge id");
    release_resolve_call(&call);
    call = resolve_with(FAKE_ID, NULL, NULL, NULL);
    check_refused(call.status, SPISOR_MALFORMED, &call.refusal, "no at", "no at");
    release_resolve_call(&call);
    call = resolve_with(FAKE_ID, bad_at, NULL, NULL);
    check_refused(call.status, SPISOR_MALFORMED, &call.refusal, "forge:zero", "a place that is none");
    release_resolve_call(&call);
    call = resolve_with(FAKE_ID, at, "nowhere", NULL);
    check_refused(call.status, SPISOR_MALFORMED, &call.refusal, "accepted primary", "an accepted primary that is no place");
    release_resolve_call(&call);
    snprintf(cache, sizeof cache, "relative/cache");
    call = resolve_with(FAKE_ID, at, NULL, NULL);
    check_refused(call.status, SPISOR_MALFORMED, &call.refusal, "cache", "a relative cache");
    release_resolve_call(&call);
}

static void a_token_goes_only_with_its_host(void) {
    runtime_identity_resolve_argument argument = SPISOR_INIT(runtime_identity_resolve_argument);
    runtime_identity_resolution answer = SPISOR_INIT(runtime_identity_resolution);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    const char *at[] = {"forge:10"};
    argument.id = FAKE_ID;
    argument.at = at;
    argument.at_count = 1;
    argument.token = "secret";
    check_refused(identity()->resolve(identity()->context, &argument, &answer, &refusal), SPISOR_MALFORMED, &refusal,
                  "token", "a token with no host");
    identity()->release_refusal(identity()->context, &refusal);
    argument.token_host = "github.com";
    argument.token = "two words";
    check_refused(identity()->resolve(identity()->context, &argument, &answer, &refusal), SPISOR_MALFORMED, &refusal,
                  "token", "a token with a space");
    identity()->release_refusal(identity()->context, &refusal);
}

static void a_short_argument_or_a_filled_answer_is_malformed(void) {
    runtime_identity_id_argument argument = SPISOR_INIT(runtime_identity_id_argument);
    runtime_identity_parts parts = SPISOR_INIT(runtime_identity_parts);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    argument.id = FAKE_ID;
    argument.size = sizeof argument.size;
    check_refused(identity()->parse(identity()->context, &argument, &parts, &refusal), SPISOR_MALFORMED, &refusal,
                  "short of its first shape", "a short argument");
    identity()->release_refusal(identity()->context, &refusal);
    argument.size = sizeof argument;
    parts.storage = &parts;
    check_refused(identity()->parse(identity()->context, &argument, &parts, &refusal), SPISOR_MALFORMED, &refusal,
                  "short of its first shape", "an answer already holding storage");
    identity()->release_refusal(identity()->context, &refusal);
}

int main(void) {
    CHECK(identity()->size == sizeof(runtime_identity_definition));
    fake_forge_install();
    the_grammar_is_naulas();
    a_git_id_parses_to_its_commit_and_path();
    a_local_id_has_a_name_and_no_commit();
    an_invalid_id_answers_none_and_no_parts();
    a_parse_without_an_id_is_malformed();
    a_folder_swaps_the_colon_for_a_plus_and_the_slash_for_an_equals_sign();
    a_folder_no_portable_name_or_one_id_alone_can_carry_is_refused();
    a_repo_resolves_to_the_homes_that_list_each_other();
    a_fork_is_refused_as_a_copy();
    an_unknown_provider_is_refused_with_the_plugin_fix();
    a_changed_primary_needs_the_user_until_accepted();
    a_disputed_number_needs_the_user_until_accepted();
    a_refusal_that_asks_nothing_wins_over_a_declined_acceptance();
    a_resolve_without_what_it_needs_is_malformed();
    a_token_goes_only_with_its_host();
    a_short_argument_or_a_filled_answer_is_malformed();
    return check_summary("identity_test");
}
