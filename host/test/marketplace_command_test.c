#include "host_fixture.h"
#include "spisor/allocation.h"

#include "host_commands.h"
#include "host_output.h"
#include "fake_forge.h"
#include "marketplace_check.h"

#define TOKEN "tok-1234567890abcdef"
#define TOKEN_VARIABLE "SPISOR_MARKETPLACE_TEST_TOKEN"
#define HOSTED_HOME(account) "{\"provider\":\"forge\",\"host\":\"git.example.com\",\"account\":\"" account "\"}"
#define KEPT_COPY \
    "{\"format\":1,\"built\":\"2026-11-01T12:00:00Z\",\"entries\":[{\"id\":\"git:000000000000000000000000000000000000003c\",\"at\":[\"forge:60\"],\"name\":\"kept\",\"fullName\":\"spisor/kept\"}]}"

static char base_folder[1024];

static char *in_base(const char *rest) {
    return joined(base_folder, rest);
}

static void start_case(const char *label) {
    snprintf(base_folder, sizeof base_folder, "%s/marketplace-command-%d-%s", RUNTIME_SCRATCH_PATH, (int)fixture_process_id(), label);
    start_empty_directory(base_folder);
}

static void finish_case(void) {
    CHECK(runtime_remove_tree(base_folder) == SPISOR_OK);
}

static int run_command(const char *list_path, const char *out_path) {
    const char *arguments[] = {"--list", list_path, "--out", out_path};
    return host_command_marketplace_build(4, (char **)arguments);
}

static int count_files_in(const char *directory) {
    runtime_directory_listing listing;
    int listed = runtime_list_directory(directory, &listing) == SPISOR_OK;
    int count = listed ? (int)listing.count : -1;
    runtime_free_directory_listing(&listing);
    return count;
}

static void writes_the_copy_atomically_and_exits_0(void) {
    start_case("writes");
    write_in(base_folder, "marketplace.json", "{\"format\":1,\"title\":\"Spisor\",\"entries\":[{\"id\":\"git:0000000000000000000000000000000000000001\",\"at\":[\"forge:1\"]},{\"id\":\"git:0000000000000000000000000000000000000002\",\"at\":[\"forge:2\"],\"alias\":\"core\"}]}");
    char *list = in_base("marketplace.json");
    char *out = in_base("published/copy.json");
    CHECK(run_command(list, out) == 0);
    cJSON *copy = json_in(base_folder, "published/copy.json");
    char problem[256];
    CHECK(runtime_marketplace_check_copy(copy, problem, sizeof problem));
    check_text(text_at(copy, "title"), "Spisor", "title");
    CHECK(runtime_marketplace_is_built_time(text_at(copy, "built")));
    const cJSON *entries = cJSON_GetObjectItemCaseSensitive(copy, "entries");
    check_text(text_at(cJSON_GetArrayItem(entries, 0), "name"), "runtime", "current name");
    check_text(text_at(cJSON_GetArrayItem(entries, 1), "name"), "core", "alias");
    char *published = in_base("published");
    CHECK(count_files_in(published) == 1);
    CHECK(run_command(list, out) == 0);
    CHECK(count_files_in(published) == 1);
    spisor_memory_release(published);
    cJSON_Delete(copy);
    spisor_memory_release(out);
    spisor_memory_release(list);
    finish_case();
}

static void a_skipped_entry_is_reported_and_the_copy_still_written(void) {
    start_case("skipped");
    write_in(base_folder, "list.json", "{\"format\":1,\"entries\":[{\"id\":\"git:0000000000000000000000000000000000000002\",\"at\":[\"forge:2\"]},{\"id\":\"git:0000000000000000000000000000000000000006\",\"at\":[\"forge:6\"]}]}");
    char *list = in_base("list.json");
    char *out = in_base("copy.json");
    CHECK(run_command(list, out) == 0);
    cJSON *copy = json_in(base_folder, "copy.json");
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(copy, "entries")) == 1);
    cJSON_Delete(copy);
    spisor_memory_release(out);
    spisor_memory_release(list);
    finish_case();
}

static void a_refusal_exits_non_zero_and_writes_nothing(void) {
    start_case("refuses");
    write_in(base_folder, "clash.json", "{\"format\":1,\"entries\":[{\"id\":\"git:0000000000000000000000000000000000000001\",\"at\":[\"forge:1\"]},{\"id\":\"git:0000000000000000000000000000000000000004\",\"at\":[\"forge:4\"]}]}");
    write_in(base_folder, "broken.json", "{\"format\":1");
    write_in(base_folder, "kept/copy.json", KEPT_COPY);
    char *kept = in_base("kept/copy.json");
    const char *lists[] = {"clash.json", "broken.json", "absent.json"};
    for (size_t index = 0; index < sizeof lists / sizeof lists[0]; index++) {
        char *list = in_base(lists[index]);
        char *fresh = in_base("fresh/copy.json");
        CHECK(run_command(list, fresh) == 1);
        CHECK(!exists_in(base_folder, "fresh"));
        CHECK(run_command(list, kept) == 1);
        char *content = text_in(base_folder, "kept/copy.json");
        check_text(content, KEPT_COPY, "an existing copy is left alone");
        spisor_memory_release(content);
        spisor_memory_release(fresh);
        spisor_memory_release(list);
    }
    spisor_memory_release(kept);
    finish_case();
}

static void answers_2_when_the_list_or_the_out_is_missing(void) {
    const char *without_out[] = {"--list", "x"};
    CHECK(host_command_marketplace_build(2, (char **)without_out) == 2);
    const char *without_list[] = {"--out", "x"};
    CHECK(host_command_marketplace_build(2, (char **)without_list) == 2);
    const char *unknown[] = {"--list", "x", "--out", "y", "--extra", "z"};
    CHECK(host_command_marketplace_build(6, (char **)unknown) == 2);
}

static void prints_a_name_with_its_control_characters_escaped(void) {
    char *escaped = host_printable("a\x1b[2Jb\x7f" "c\xc2\x9b" "d");
    check_text(escaped, "a\\x1b[2Jb\\x7f" "c\\u009bd", "escaped text");
    spisor_memory_release(escaped);
}

static int run_binary(const char *list_path, program *run) {
    prepare_program(run, base_folder, "build");
    const char *words[] = {"marketplace", "build", "--list", list_path, "--out", "unused/copy.json", NULL};
    cJSON *arguments = strings_of(words);
    cJSON *environment = cJSON_CreateObject();
    copy_system_variable(environment, "SystemRoot");
    copy_system_variable(environment, "PATH");
    program_start(run, SPISOR_HOST_PATH, arguments, environment, base_folder, 1);
    cJSON_Delete(environment);
    cJSON_Delete(arguments);
    return program_finish(run, 60000);
}

static void the_binary_refuses_a_local_id_and_escapes_it_on_the_terminal(void) {
    start_case("binary");
    write_in(base_folder, "local.json", "{\"format\":1,\"entries\":[{\"id\":\"local:a\\u001b[2Jb\",\"at\":[\"forge:1\"]}]}");
    char *list = in_base("local.json");
    program run;
    CHECK(run_binary(list, &run) == 1);
    expect_contains(run.err, "local:a\\x1b[2Jb is a local id", "the id, escaped");
    expect_contains(run.err, "fix: ", "its fix");
    CHECK(strchr(run.err, '\x1b') == NULL);
    CHECK(!exists_in(base_folder, "unused"));
    program_free(&run);
    spisor_memory_release(list);
    finish_case();
}

static void set_token_variable(const char *value) {
#if defined(_WIN32)
    _putenv_s(TOKEN_VARIABLE, value == NULL ? "" : value);
#elif defined(__unix__) || defined(__APPLE__)
    if (value == NULL) {
        unsetenv(TOKEN_VARIABLE);
    } else {
        setenv(TOKEN_VARIABLE, value, 1);
    }
#endif
}

static int run_command_with_token(const char *list_path, const char *out_path) {
    const char *arguments[] = {"--list", list_path, "--out", out_path, "--token-env", TOKEN_VARIABLE, "--token-host", "git.example.com"};
    return host_command_marketplace_build(8, (char **)arguments);
}

static int run_command_with_token_and_no_host(const char *list_path, const char *out_path) {
    const char *arguments[] = {"--list", list_path, "--out", out_path, "--token-env", TOKEN_VARIABLE};
    return host_command_marketplace_build(6, (char **)arguments);
}

static void an_unresolvable_entry_is_carried_over_or_skipped_and_the_copy_still_written(void) {
    start_case("unresolvable");
    write_in(base_folder, "list.json", "{\"format\":1,\"entries\":[{\"id\":\"git:0000000000000000000000000000000000000001\",\"at\":[\"forge:1\"]},{\"id\":\"git:000000000000000000000000000000000000003c\",\"at\":[\"forge:60\"]}]}");
    write_in(base_folder, "carried/copy.json", KEPT_COPY);
    char *list = in_base("list.json");
    char *carried = in_base("carried/copy.json");
    char *fresh = in_base("fresh/copy.json");
    CHECK(run_command(list, carried) == 0);
    cJSON *kept = json_in(base_folder, "carried/copy.json");
    const cJSON *entries = cJSON_GetObjectItemCaseSensitive(kept, "entries");
    CHECK(cJSON_GetArraySize(entries) == 2);
    check_text(text_at(cJSON_GetArrayItem(entries, 1), "name"), "kept", "carried over from the copy at --out");
    cJSON_Delete(kept);
    CHECK(run_command(list, fresh) == 0);
    cJSON *skipped = json_in(base_folder, "fresh/copy.json");
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(skipped, "entries")) == 1);
    cJSON_Delete(skipped);
    spisor_memory_release(fresh);
    spisor_memory_release(carried);
    spisor_memory_release(list);
    finish_case();
}

static void a_copy_at_out_that_is_not_a_copy_is_refused_and_left_alone(void) {
    start_case("bad-previous");
    write_in(base_folder, "list.json", "{\"format\":1,\"entries\":[{\"id\":\"git:0000000000000000000000000000000000000001\",\"at\":[\"forge:1\"]}]}");
    write_in(base_folder, "a.json", "{\"format\":1}");
    write_in(base_folder, "b.json", "not json");
    char *list = in_base("list.json");
    const char *outs[] = {"a.json", "b.json"};
    for (size_t index = 0; index < 2; index++) {
        char *out = in_base(outs[index]);
        CHECK(run_command(list, out) == 1);
        char *content = text_in(base_folder, outs[index]);
        CHECK(strcmp(content, index == 0 ? "{\"format\":1}" : "not json") == 0);
        spisor_memory_release(content);
        spisor_memory_release(out);
    }
    spisor_memory_release(list);
    finish_case();
}

static void out_that_is_the_list_is_refused_and_the_list_left_alone(void) {
    start_case("out-is-list");
    const char *text = "{\"format\":1,\"entries\":[{\"id\":\"git:0000000000000000000000000000000000000001\",\"at\":[\"forge:1\"]}]}";
    write_in(base_folder, "marketplace.json", text);
    write_in(base_folder, "sub/placeholder", "");
    char *list = in_base("marketplace.json");
    char *spelled_differently = in_base("sub/../marketplace.json");
    CHECK(run_command(list, list) == 1);
    CHECK(run_command(list, spelled_differently) == 1);
    char *content = text_in(base_folder, "marketplace.json");
    CHECK(strcmp(content, text) == 0);
    spisor_memory_release(content);
    spisor_memory_release(spelled_differently);
    spisor_memory_release(list);
    finish_case();
}

static void the_token_in_the_named_variable_reaches_each_lookup_and_is_written_nowhere(void) {
    start_case("token");
    write_in(base_folder, "list.json", "{\"format\":1,\"entries\":[{\"id\":\"git:0000000000000000000000000000000000000001\",\"at\":[\"forge@git.example.com:11\"]},{\"id\":\"git:0000000000000000000000000000000000000002\",\"at\":[\"forge@git.example.com:12\"]}]}");
    char *list = in_base("list.json");
    char *out = in_base("copy.json");
    unsigned before = fake_forge_calls_with_token(TOKEN);
    set_token_variable(TOKEN);
    CHECK(run_command_with_token(list, out) == 0);
    set_token_variable(NULL);
    CHECK(fake_forge_calls_with_token(TOKEN) > before);
    char *content = text_in(base_folder, "copy.json");
    CHECK(content != NULL && strstr(content, TOKEN) == NULL && strstr(content, "spisor/runtime") != NULL);
    spisor_memory_release(content);
    spisor_memory_release(out);
    spisor_memory_release(list);
    finish_case();
}

static void a_variable_that_is_not_set_is_refused_and_writes_nothing(void) {
    start_case("unset-token");
    write_in(base_folder, "list.json", "{\"format\":1,\"entries\":[{\"id\":\"git:0000000000000000000000000000000000000001\",\"at\":[\"forge:1\"]}]}");
    char *list = in_base("list.json");
    char *out = in_base("fresh/copy.json");
    set_token_variable(NULL);
    CHECK(run_command_with_token(list, out) == 1);
    CHECK(!exists_in(base_folder, "fresh"));
    spisor_memory_release(out);
    spisor_memory_release(list);
    finish_case();
}

static void a_token_without_its_host_is_refused_and_writes_nothing(void) {
    start_case("token-no-host");
    write_in(base_folder, "list.json", "{\"format\":1,\"entries\":[{\"id\":\"git:0000000000000000000000000000000000000001\",\"at\":[\"forge:1\"]}]}");
    char *list = in_base("list.json");
    char *out = in_base("fresh/copy.json");
    set_token_variable(TOKEN);
    CHECK(run_command_with_token_and_no_host(list, out) == 2);
    set_token_variable(NULL);
    CHECK(!exists_in(base_folder, "fresh"));
    spisor_memory_release(out);
    spisor_memory_release(list);
    finish_case();
}

static void an_unusable_token_is_refused_and_writes_nothing(void) {
    start_case("unusable-token");
    write_in(base_folder, "list.json", "{\"format\":1,\"entries\":[{\"id\":\"git:0000000000000000000000000000000000000001\",\"at\":[\"forge:1\"]}]}");
    char *list = in_base("list.json");
    char *out = in_base("fresh/copy.json");
    set_token_variable(TOKEN "\n");
    CHECK(run_command_with_token(list, out) == 1);
    set_token_variable(NULL);
    CHECK(!exists_in(base_folder, "fresh"));
    spisor_memory_release(out);
    spisor_memory_release(list);
    finish_case();
}

static void a_build_where_every_lookup_is_refused_authorization_writes_nothing(void) {
    start_case("refused-authorization");
    write_in(base_folder, "list.json", "{\"format\":1,\"entries\":[{\"id\":\"git:0000000000000000000000000000000000000046\",\"at\":[\"forge:70\"]}]}");
    write_in(base_folder, "carried/copy.json", KEPT_COPY);
    char *list = in_base("list.json");
    char *carried = in_base("carried/copy.json");
    char *fresh = in_base("fresh/copy.json");
    CHECK(run_command(list, carried) == 1);
    CHECK(run_command(list, fresh) == 1);
    char *content = text_in(base_folder, "carried/copy.json");
    CHECK(strcmp(content, KEPT_COPY) == 0);
    spisor_memory_release(content);
    CHECK(!exists_in(base_folder, "fresh"));
    spisor_memory_release(fresh);
    spisor_memory_release(carried);
    spisor_memory_release(list);
    finish_case();
}

static int run_binary_with_token(const char *list_path, program *run) {
    prepare_program(run, base_folder, "token");
    const char *words[] = {"marketplace", "build", "--list", list_path, "--out", "unused/copy.json", "--token-env", TOKEN_VARIABLE, "--token-host", "git.example.com", NULL};
    cJSON *arguments = strings_of(words);
    cJSON *environment = cJSON_CreateObject();
    copy_system_variable(environment, "SystemRoot");
    copy_system_variable(environment, "PATH");
    cJSON_AddStringToObject(environment, TOKEN_VARIABLE, TOKEN);
    program_start(run, SPISOR_HOST_PATH, arguments, environment, base_folder, 1);
    cJSON_Delete(environment);
    cJSON_Delete(arguments);
    return program_finish(run, 60000);
}

static void the_binary_never_prints_the_token(void) {
    start_case("binary-token");
    write_in(base_folder, "local.json", "{\"format\":1,\"entries\":[{\"id\":\"local:a\",\"at\":[\"forge:1\"]}]}");
    char *list = in_base("local.json");
    program run;
    CHECK(run_binary_with_token(list, &run) == 1);
    expect_contains(run.err, "local:a is a local id", "the refusal");
    CHECK(strstr(run.err, TOKEN) == NULL && strstr(run.out, TOKEN) == NULL);
    program_free(&run);
    spisor_memory_release(list);
    finish_case();
}

static fake_repo *add(uint64_t number, const char *account, const char *name, const char *root, const char *homes) {
    fake_repo *repo = fake_forge_add(number, account, name, fake_forge_file(name, homes));
    repo->root = root;
    return repo;
}

/** The repos the lists name: two on the fake provider's own host, the same two at git.example.com, and two awkward. */
static void run_against_the_forge(void) {
    static const char runtime_root[] = "0000000000000000000000000000000000000001";
    static const char weivel_root[] = "0000000000000000000000000000000000000002";
    fake_forge_install();
    fake_forge_reset();
    add(1, "spisor", "runtime", runtime_root, FAKE_HOME("spisor"));
    add(2, "spisor", "weivel", weivel_root, FAKE_HOME("spisor"));
    add(4, "other", "runtime", "0000000000000000000000000000000000000004", FAKE_HOME("other"));
    add(6, "spisor", "Con", "0000000000000000000000000000000000000006", FAKE_HOME("spisor"));
    add(70, "spisor", "locked", "0000000000000000000000000000000000000046", FAKE_HOME("spisor"))->refused_with =
        "token_needed";
    add(11, "spisor", "runtime", runtime_root, HOSTED_HOME("spisor"));
    add(12, "spisor", "weivel", weivel_root, HOSTED_HOME("spisor"));
    writes_the_copy_atomically_and_exits_0();
    a_skipped_entry_is_reported_and_the_copy_still_written();
    a_refusal_exits_non_zero_and_writes_nothing();
    an_unresolvable_entry_is_carried_over_or_skipped_and_the_copy_still_written();
    a_copy_at_out_that_is_not_a_copy_is_refused_and_left_alone();
    out_that_is_the_list_is_refused_and_the_list_left_alone();
    the_token_in_the_named_variable_reaches_each_lookup_and_is_written_nowhere();
    a_variable_that_is_not_set_is_refused_and_writes_nothing();
    a_token_without_its_host_is_refused_and_writes_nothing();
    an_unusable_token_is_refused_and_writes_nothing();
    a_build_where_every_lookup_is_refused_authorization_writes_nothing();
}

int main(void) {
    run_against_the_forge();
    answers_2_when_the_list_or_the_out_is_missing();
    prints_a_name_with_its_control_characters_escaped();
    the_binary_refuses_a_local_id_and_escapes_it_on_the_terminal();
    the_binary_never_prints_the_token();
    return check_summary("marketplace_command_test");
}
