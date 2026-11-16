#include <stdio.h>
#include <string.h>

#include "check.h"
#include "definition_fixture.h"
#include "runtime/identity.h"

typedef struct identity_vector {
    const char *name;
    const char *value;
    int valid;
} identity_vector;

/** The values of weivel/conformance/vectors/identity/, one row per vector file. */
static const identity_vector kit_vectors[] = {
    {"accepts-a-gitea-host-qualified-id", "gitea@git.example.com:123", 1},
    {"accepts-a-github-repository-id", "github:1434954468", 1},
    {"accepts-a-local-id", "local:plugin-stub", 1},
    {"accepts-a-path-under-a-repository-id", "github:1361969293/kernel", 1},
    {"accepts-a-segment-containing-two-dots", "github:1/a..b", 1},
    {"accepts-a-segment-starting-with-a-dot", "github:1/.hidden", 1},
    {"refuses-a-bare-name", "calendar", 0},
    {"refuses-a-dot-dot-segment-before-a-name", "github:1/../calendar_identity", 0},
    {"refuses-a-dot-dot-segment-between-names", "github:1/a/../b", 0},
    {"refuses-a-dot-dot-segment", "github:1/..", 0},
    {"refuses-a-non-numeric-repository-id", "github:abc", 0},
    {"refuses-a-single-dot-segment", "github:1/.", 0},
    {"refuses-an-empty-repository-id", "github:", 0},
    {"refuses-an-uppercase-forge", "GitHub:1", 0},
};

static const char *const further_invalid[] = {
    "",           "local:",       "local:a b",   "local:a\tb",   "local:/x",  "local:x/",    "local:x/A",
    "github:1/",  "github:1//a",  "github:1/A",  "github:1/a b", "github:1a", "github@:1",   "github@HOST:1",
    "gitea@h@i:1", "1github:1",   "-github:1",   "github:-1",    "github:1 ", " github:1",   "github:1/a/.",
    "github:1/./a", "local:x/..", "local:x/./y", "local:x\n",
};

static const char *const further_valid[] = {
    "local:a", "local:a:b", "local:12", "local:a\\b", "local:x/y", "local:caf\xc3\xa9", "local:..",
    "github:0", "github:1/a/b/c", "github:1/a_b-c.d", "gitea@a.b-c:7", "a1-b:9/x", "local:x/..a", "local:x/a..",
};

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
    CHECK((status == SPISOR_OK) == (parts.resolver != NULL));
    identity()->release_parts(identity()->context, &parts);
    return status == SPISOR_OK;
}

static void every_kit_vector_gives_the_same_verdict(void) {
    for (size_t index = 0; index < sizeof kit_vectors / sizeof kit_vectors[0]; index++) {
        if (is_valid(kit_vectors[index].value) != kit_vectors[index].valid) {
            fprintf(stderr, "%s: \"%s\" should be %s\n", kit_vectors[index].name, kit_vectors[index].value,
                    kit_vectors[index].valid ? "valid" : "invalid");
            check_failures++;
        }
    }
}

static void further_shapes_the_grammar_decides(void) {
    for (size_t index = 0; index < sizeof further_invalid / sizeof further_invalid[0]; index++) {
        if (is_valid(further_invalid[index])) {
            fprintf(stderr, "\"%s\" should be invalid\n", further_invalid[index]);
            check_failures++;
        }
    }
    for (size_t index = 0; index < sizeof further_valid / sizeof further_valid[0]; index++) {
        if (!is_valid(further_valid[index])) {
            fprintf(stderr, "\"%s\" should be valid\n", further_valid[index]);
            check_failures++;
        }
    }
}

static void an_invalid_id_answers_none_and_no_parts(void) {
    runtime_identity_parts parts = SPISOR_INIT(runtime_identity_parts);
    CHECK(parse_into("github:1/..", &parts) == SPISOR_NONE);
    CHECK(parts.resolver == NULL && parts.host == NULL && parts.number == NULL && parts.path == NULL);
    CHECK(parts.storage == NULL);
}

static void a_github_id_parses_to_its_parts(void) {
    runtime_identity_parts parts = SPISOR_INIT(runtime_identity_parts);
    CHECK(parse_into("github:12/kernel", &parts) == SPISOR_OK);
    check_text(parts.resolver, "github", "resolver");
    check_text(parts.number, "12", "number");
    check_text(parts.path, "kernel", "path");
    CHECK(parts.host == NULL);
    identity()->release_parts(identity()->context, &parts);
    CHECK(parts.storage == NULL && parts.resolver == NULL);
}

static void a_deep_path_is_kept_whole(void) {
    runtime_identity_parts parts = SPISOR_INIT(runtime_identity_parts);
    CHECK(parse_into("github:12/a/b.c", &parts) == SPISOR_OK);
    check_text(parts.path, "a/b.c", "deep path");
    identity()->release_parts(identity()->context, &parts);
}

static void an_id_without_a_path_has_no_path(void) {
    runtime_identity_parts parts = SPISOR_INIT(runtime_identity_parts);
    CHECK(parse_into("github:12", &parts) == SPISOR_OK);
    CHECK(parts.path == NULL);
    check_text(parts.number, "12", "number");
    identity()->release_parts(identity()->context, &parts);
}

static void a_host_qualified_id_carries_its_host(void) {
    runtime_identity_parts parts = SPISOR_INIT(runtime_identity_parts);
    CHECK(parse_into("gitea@git.example.com:5", &parts) == SPISOR_OK);
    check_text(parts.resolver, "gitea", "resolver");
    check_text(parts.host, "git.example.com", "host");
    check_text(parts.number, "5", "number");
    identity()->release_parts(identity()->context, &parts);
}

static void a_local_id_resolves_locally_with_no_number(void) {
    runtime_identity_parts parts = SPISOR_INIT(runtime_identity_parts);
    CHECK(parse_into("local:x", &parts) == SPISOR_OK);
    check_text(parts.resolver, "local", "resolver");
    check_text(parts.path, "x", "local name");
    CHECK(parts.number == NULL);
    CHECK(parts.host == NULL);
    identity()->release_parts(identity()->context, &parts);
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
    if (!check_refused(call.status, expected, &call.refusal, NULL, id) || call.answer.folder != NULL) {
        fprintf(stderr, "%s should have no folder\n", id);
        check_failures++;
    }
    release_folder_call(&call);
}

static void a_folder_swaps_the_colon_for_a_plus_and_the_slash_for_an_equals_sign(void) {
    expect_folder("github:12/kernel", "github+12=kernel");
    expect_folder("gitea@git.example.com:5", "gitea@git.example.com+5");
    expect_folder("local:plugin-stub", "local+plugin-stub");
}

static void no_folder_holds_a_path_separator_or_colon(void) {
    for (size_t index = 0; index < sizeof kit_vectors / sizeof kit_vectors[0]; index++) {
        folder_call call = folder_of(kit_vectors[index].value);
        const char *folder = call.answer.folder;
        CHECK((folder != NULL) == kit_vectors[index].valid);
        CHECK(folder == NULL || strpbrk(folder, ":/\\") == NULL);
        release_folder_call(&call);
    }
    for (size_t index = 0; index < sizeof further_valid / sizeof further_valid[0]; index++) {
        folder_call call = folder_of(further_valid[index]);
        CHECK(call.answer.folder == NULL || strpbrk(call.answer.folder, ":/\\") == NULL);
        release_folder_call(&call);
    }
}

static void a_name_a_folder_cannot_carry_is_refused_not_rewritten(void) {
    expect_no_folder("local:a\\b", SPISOR_REFUSED);
    expect_no_folder("local:a?", SPISOR_REFUSED);
    expect_no_folder("github:1/a.", SPISOR_REFUSED);
}

static void a_local_id_whose_folder_would_be_shared_is_refused(void) {
    const char *const colliding[] = {"local:a=b", "local:a+b", "local:a:b", "local:Foo", "local:fOo"};
    for (size_t index = 0; index < sizeof colliding / sizeof colliding[0]; index++) {
        expect_no_folder(colliding[index], SPISOR_REFUSED);
    }
    expect_no_folder("local:x/a+b", SPISOR_MALFORMED);
    expect_folder("local:a/b", "local+a=b");
    expect_folder("local:foo", "local+foo");
    CHECK(is_valid("local:a=b") && is_valid("local:Foo"));
}

static void a_folder_for_an_invalid_id_is_malformed(void) {
    expect_no_folder("calendar", SPISOR_MALFORMED);
    expect_no_folder("github:1/..", SPISOR_MALFORMED);
    expect_no_folder(NULL, SPISOR_MALFORMED);
}

static spisor_status accepts(const char *recorded, const char *primary, runtime_identity_version_answer *answer,
                             spisor_refusal *refusal) {
    runtime_identity_version_argument argument = SPISOR_INIT(runtime_identity_version_argument);
    argument.recorded = recorded;
    argument.primary = primary;
    return identity()->accepts_version(identity()->context, &argument, answer, refusal);
}

static void only_the_recorded_primary_is_accepted_without_the_user(void) {
    runtime_identity_version_answer answer = SPISOR_INIT(runtime_identity_version_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    CHECK(accepts("github:1", "github:1", &answer, &refusal) == SPISOR_OK && answer.accepted == 1);
    CHECK(accepts("github:1", "github:2", &answer, &refusal) == SPISOR_OK && answer.accepted == 0);
    check_refused(accepts("github:1/x", "github:1", &answer, &refusal), SPISOR_MALFORMED, &refusal, "recorded",
                  "a recorded id with a folder");
    identity()->release_refusal(identity()->context, &refusal);
    check_refused(accepts("github:1", "local:x", &answer, &refusal), SPISOR_MALFORMED, &refusal, "primary",
                  "a local primary");
    identity()->release_refusal(identity()->context, &refusal);
}

static void a_short_argument_or_a_filled_answer_is_malformed(void) {
    runtime_identity_id_argument argument = SPISOR_INIT(runtime_identity_id_argument);
    runtime_identity_parts parts = SPISOR_INIT(runtime_identity_parts);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    argument.id = "github:1";
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
    every_kit_vector_gives_the_same_verdict();
    further_shapes_the_grammar_decides();
    an_invalid_id_answers_none_and_no_parts();
    a_github_id_parses_to_its_parts();
    a_deep_path_is_kept_whole();
    an_id_without_a_path_has_no_path();
    a_host_qualified_id_carries_its_host();
    a_local_id_resolves_locally_with_no_number();
    a_parse_without_an_id_is_malformed();
    a_folder_swaps_the_colon_for_a_plus_and_the_slash_for_an_equals_sign();
    no_folder_holds_a_path_separator_or_colon();
    a_name_a_folder_cannot_carry_is_refused_not_rewritten();
    a_local_id_whose_folder_would_be_shared_is_refused();
    a_folder_for_an_invalid_id_is_malformed();
    only_the_recorded_primary_is_accepted_without_the_user();
    a_short_argument_or_a_filled_answer_is_malformed();
    return check_summary("identity_test");
}
