#include <stdio.h>
#include <string.h>

#include "catalog_call.h"
#include "check.h"
#include "identity.h"

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

static cJSON *call_with_id(catalog_function function, const char *id) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "id", id);
    catalog_answer answer = call_catalog(function, argument);
    if (answer.status != SPISOR_STATUS_SUCCESS) {
        cJSON_Delete(answer.tree);
        return NULL;
    }
    return answer.tree;
}

static int is_valid(const char *id) {
    cJSON *answer = call_with_id(runtime_identity_parse, id);
    const cJSON *valid = cJSON_GetObjectItemCaseSensitive(answer, "valid");
    int result = cJSON_IsTrue(valid);
    CHECK(cJSON_IsBool(valid));
    cJSON_Delete(answer);
    return result;
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

static void an_invalid_id_answers_only_valid_false(void) {
    cJSON *answer = call_with_id(runtime_identity_parse, "github:1/..");
    char *text = cJSON_PrintUnformatted(answer);
    check_text(text, "{\"valid\":false}", "invalid answer");
    cJSON_free(text);
    cJSON_Delete(answer);
}

static void a_github_id_parses_to_its_parts(void) {
    cJSON *answer = call_with_id(runtime_identity_parse, "github:12/kernel");
    check_text(text_at(answer, "resolver"), "github", "resolver");
    check_text(text_at(answer, "number"), "12", "number");
    check_text(text_at(answer, "path"), "kernel", "path");
    CHECK(cJSON_GetObjectItemCaseSensitive(answer, "host") == NULL);
    cJSON_Delete(answer);
}

static void a_deep_path_is_kept_whole(void) {
    cJSON *answer = call_with_id(runtime_identity_parse, "github:12/a/b.c");
    check_text(text_at(answer, "path"), "a/b.c", "deep path");
    cJSON_Delete(answer);
}

static void an_id_without_a_path_has_no_path(void) {
    cJSON *answer = call_with_id(runtime_identity_parse, "github:12");
    CHECK(cJSON_GetObjectItemCaseSensitive(answer, "path") == NULL);
    check_text(text_at(answer, "number"), "12", "number");
    cJSON_Delete(answer);
}

static void a_host_qualified_id_carries_its_host(void) {
    cJSON *answer = call_with_id(runtime_identity_parse, "gitea@git.example.com:5");
    check_text(text_at(answer, "resolver"), "gitea", "resolver");
    check_text(text_at(answer, "host"), "git.example.com", "host");
    check_text(text_at(answer, "number"), "5", "number");
    cJSON_Delete(answer);
}

static void a_local_id_resolves_locally_with_no_number(void) {
    cJSON *answer = call_with_id(runtime_identity_parse, "local:x");
    check_text(text_at(answer, "resolver"), "local", "resolver");
    check_text(text_at(answer, "path"), "x", "local name");
    CHECK(cJSON_GetObjectItemCaseSensitive(answer, "number") == NULL);
    CHECK(cJSON_GetObjectItemCaseSensitive(answer, "host") == NULL);
    cJSON_Delete(answer);
}

static void expect_folder(const char *id, const char *expected) {
    cJSON *answer = call_with_id(runtime_identity_folder, id);
    check_text(text_at(answer, "folder"), expected, id);
    cJSON_Delete(answer);
}

static void a_folder_swaps_the_colon_for_a_plus_and_the_slash_for_an_equals_sign(void) {
    expect_folder("github:12/kernel", "github+12=kernel");
    expect_folder("gitea@git.example.com:5", "gitea@git.example.com+5");
    expect_folder("local:plugin-stub", "local+plugin-stub");
}

static void no_folder_holds_a_path_separator_or_colon(void) {
    for (size_t index = 0; index < sizeof kit_vectors / sizeof kit_vectors[0]; index++) {
        cJSON *answer = call_with_id(runtime_identity_folder, kit_vectors[index].value);
        const char *folder = text_at(answer, "folder");
        CHECK((folder != NULL) == kit_vectors[index].valid);
        CHECK(folder == NULL || strpbrk(folder, ":/\\") == NULL);
        cJSON_Delete(answer);
    }
    for (size_t index = 0; index < sizeof further_valid / sizeof further_valid[0]; index++) {
        cJSON *answer = call_with_id(runtime_identity_folder, further_valid[index]);
        const char *folder = text_at(answer, "folder");
        CHECK(folder == NULL || strpbrk(folder, ":/\\") == NULL);
        cJSON_Delete(answer);
    }
}

static void a_name_a_folder_cannot_carry_is_refused_not_rewritten(void) {
    CHECK(call_with_id(runtime_identity_folder, "local:a\\b") == NULL);
    CHECK(call_with_id(runtime_identity_folder, "local:a?") == NULL);
    CHECK(call_with_id(runtime_identity_folder, "github:1/a.") == NULL);
}

static void a_folder_for_an_invalid_id_is_refused(void) {
    CHECK(call_with_id(runtime_identity_folder, "calendar") == NULL);
    CHECK(call_with_id(runtime_identity_folder, "github:1/..") == NULL);
}

int main(void) {
    every_kit_vector_gives_the_same_verdict();
    further_shapes_the_grammar_decides();
    an_invalid_id_answers_only_valid_false();
    a_github_id_parses_to_its_parts();
    a_deep_path_is_kept_whole();
    an_id_without_a_path_has_no_path();
    a_host_qualified_id_carries_its_host();
    a_local_id_resolves_locally_with_no_number();
    a_folder_swaps_the_colon_for_a_plus_and_the_slash_for_an_equals_sign();
    no_folder_holds_a_path_separator_or_colon();
    a_name_a_folder_cannot_carry_is_refused_not_rewritten();
    a_folder_for_an_invalid_id_is_refused();
    return check_summary("identity_test");
}
