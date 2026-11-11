#include "host_fixture.h"

#include "fetch_hooks.h"
#include "host_commands.h"
#include "host_output.h"
#include "marketplace_lookup.h"
#include "http_fixture.h"
#include "marketplace_check.h"

#define REPOSITORY(identifier, name_text, full_name_text) \
    "{\"id\":" identifier ",\"name\":\"" name_text "\",\"full_name\":\"" full_name_text "\",\"default_branch\":\"main\"}"
#define ROUTE(path, status, body) {path, status, NULL, body, sizeof body - 1, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0}

#define TOKEN "tok-1234567890abcdef"
#define TOKEN_VARIABLE "SPISOR_MARKETPLACE_TEST_TOKEN"
#define AUTHORIZATION_LINE "Authorization: token " TOKEN
#define KEPT_COPY \
    "{\"format\":1,\"built\":\"2026-11-01T12:00:00Z\",\"entries\":[{\"id\":\"github:60\",\"name\":\"kept\",\"fullName\":\"spisor/kept\",\"lastKnown\":\"spisor/kept\"}]}"

static char base_folder[1024];

static char *in_base(const char *rest) {
    return joined(base_folder, rest);
}

static void start_case(const char *label) {
    snprintf(base_folder, sizeof base_folder, "%s/marketplace-command-%d-%s", RUNTIME_SCRATCH_PATH, (int)fixture_process_id(), label);
    start_empty_directory(base_folder);
}

static void finish_case(void) {
    CHECK(runtime_remove_tree(base_folder) == 1);
}

static int run_command(const char *list_path, const char *out_path) {
    const char *arguments[] = {"--list", list_path, "--out", out_path};
    return host_command_marketplace_build(4, (char **)arguments);
}

static int count_files_in(const char *directory) {
    runtime_directory_listing listing;
    int listed = runtime_list_directory(directory, &listing);
    int count = listed ? (int)listing.count : -1;
    runtime_free_directory_listing(&listing);
    return count;
}

static void writes_the_copy_atomically_and_exits_0(void) {
    start_case("writes");
    write_in(base_folder, "marketplace.json", "{\"format\":1,\"title\":\"Spisor\",\"entries\":[{\"id\":\"github:1\"},{\"id\":\"github:2\",\"alias\":\"core\"}]}");
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
    free(published);
    cJSON_Delete(copy);
    free(out);
    free(list);
    finish_case();
}

static void a_skipped_entry_is_reported_and_the_copy_still_written(void) {
    start_case("skipped");
    write_in(base_folder, "list.json", "{\"format\":1,\"entries\":[{\"id\":\"github:2\"},{\"id\":\"github:6\"}]}");
    char *list = in_base("list.json");
    char *out = in_base("copy.json");
    CHECK(run_command(list, out) == 0);
    cJSON *copy = json_in(base_folder, "copy.json");
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(copy, "entries")) == 1);
    cJSON_Delete(copy);
    free(out);
    free(list);
    finish_case();
}

static void a_refusal_exits_non_zero_and_writes_nothing(void) {
    start_case("refuses");
    write_in(base_folder, "clash.json", "{\"format\":1,\"entries\":[{\"id\":\"github:1\"},{\"id\":\"github:4\"}]}");
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
        free(content);
        free(fresh);
        free(list);
    }
    free(kept);
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
    free(escaped);
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
    write_in(base_folder, "local.json", "{\"format\":1,\"entries\":[{\"id\":\"local:a\\u001b[2Jb\"}]}");
    char *list = in_base("local.json");
    program run;
    CHECK(run_binary(list, &run) == 1);
    expect_contains(run.err, "local:a\\x1b[2Jb is a local id", "the id, escaped");
    expect_contains(run.err, "fix: ", "its fix");
    CHECK(strchr(run.err, '\x1b') == NULL);
    CHECK(!exists_in(base_folder, "unused"));
    program_free(&run);
    free(list);
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
    const char *arguments[] = {"--list", list_path, "--out", out_path, "--token-env", TOKEN_VARIABLE, "--token-host", "github.com"};
    return host_command_marketplace_build(8, (char **)arguments);
}

static int run_command_with_token_and_no_host(const char *list_path, const char *out_path) {
    const char *arguments[] = {"--list", list_path, "--out", out_path, "--token-env", TOKEN_VARIABLE};
    return host_command_marketplace_build(6, (char **)arguments);
}

static void an_unresolvable_entry_is_carried_over_or_skipped_and_the_copy_still_written(void) {
    start_case("unresolvable");
    write_in(base_folder, "list.json", "{\"format\":1,\"entries\":[{\"id\":\"github:1\"},{\"id\":\"github:60\"}]}");
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
    free(fresh);
    free(carried);
    free(list);
    finish_case();
}

static void a_copy_at_out_that_is_not_a_copy_is_refused_and_left_alone(void) {
    start_case("bad-previous");
    write_in(base_folder, "list.json", "{\"format\":1,\"entries\":[{\"id\":\"github:1\"}]}");
    write_in(base_folder, "a.json", "{\"format\":1}");
    write_in(base_folder, "b.json", "not json");
    char *list = in_base("list.json");
    const char *outs[] = {"a.json", "b.json"};
    for (size_t index = 0; index < 2; index++) {
        char *out = in_base(outs[index]);
        CHECK(run_command(list, out) == 1);
        char *content = text_in(base_folder, outs[index]);
        CHECK(strcmp(content, index == 0 ? "{\"format\":1}" : "not json") == 0);
        free(content);
        free(out);
    }
    free(list);
    finish_case();
}

static void out_that_is_the_list_is_refused_and_the_list_left_alone(void) {
    start_case("out-is-list");
    const char *text = "{\"format\":1,\"entries\":[{\"id\":\"github:1\"}]}";
    write_in(base_folder, "marketplace.json", text);
    write_in(base_folder, "sub/placeholder", "");
    char *list = in_base("marketplace.json");
    char *spelled_differently = in_base("sub/../marketplace.json");
    CHECK(run_command(list, list) == 1);
    CHECK(run_command(list, spelled_differently) == 1);
    char *content = text_in(base_folder, "marketplace.json");
    CHECK(strcmp(content, text) == 0);
    free(content);
    free(spelled_differently);
    free(list);
    finish_case();
}

static void the_token_in_the_named_variable_reaches_each_lookup_and_is_written_nowhere(void) {
    start_case("token");
    write_in(base_folder, "list.json", "{\"format\":1,\"entries\":[{\"id\":\"github:1\"},{\"id\":\"github:2\"}]}");
    char *list = in_base("list.json");
    char *out = in_base("copy.json");
    set_token_variable(TOKEN);
    CHECK(run_command_with_token(list, out) == 0);
    set_token_variable(NULL);
    char *content = text_in(base_folder, "copy.json");
    CHECK(strstr(content, TOKEN) == NULL && strstr(content, "uthorization") == NULL);
    free(content);
    free(out);
    free(list);
    finish_case();
}

static void a_variable_that_is_not_set_is_refused_and_writes_nothing(void) {
    start_case("unset-token");
    write_in(base_folder, "list.json", "{\"format\":1,\"entries\":[{\"id\":\"github:1\"}]}");
    char *list = in_base("list.json");
    char *out = in_base("fresh/copy.json");
    set_token_variable(NULL);
    CHECK(run_command_with_token(list, out) == 1);
    CHECK(!exists_in(base_folder, "fresh"));
    free(out);
    free(list);
    finish_case();
}

static void a_token_without_its_host_is_refused_and_writes_nothing(void) {
    start_case("token-no-host");
    write_in(base_folder, "list.json", "{\"format\":1,\"entries\":[{\"id\":\"github:1\"}]}");
    char *list = in_base("list.json");
    char *out = in_base("fresh/copy.json");
    set_token_variable(TOKEN);
    CHECK(run_command_with_token_and_no_host(list, out) == 2);
    set_token_variable(NULL);
    CHECK(!exists_in(base_folder, "fresh"));
    free(out);
    free(list);
    finish_case();
}

static void an_unusable_token_is_refused_and_writes_nothing(void) {
    start_case("unusable-token");
    write_in(base_folder, "list.json", "{\"format\":1,\"entries\":[{\"id\":\"github:1\"}]}");
    char *list = in_base("list.json");
    char *out = in_base("fresh/copy.json");
    set_token_variable(TOKEN "\n");
    CHECK(run_command_with_token(list, out) == 1);
    set_token_variable(NULL);
    CHECK(!exists_in(base_folder, "fresh"));
    free(out);
    free(list);
    finish_case();
}

static void a_build_where_every_lookup_is_refused_authorization_writes_nothing(void) {
    start_case("refused-authorization");
    write_in(base_folder, "list.json", "{\"format\":1,\"entries\":[{\"id\":\"github:70\"}]}");
    write_in(base_folder, "carried/copy.json", KEPT_COPY);
    char *list = in_base("list.json");
    char *carried = in_base("carried/copy.json");
    char *fresh = in_base("fresh/copy.json");
    CHECK(run_command(list, carried) == 1);
    CHECK(run_command(list, fresh) == 1);
    char *content = text_in(base_folder, "carried/copy.json");
    CHECK(strcmp(content, KEPT_COPY) == 0);
    free(content);
    CHECK(!exists_in(base_folder, "fresh"));
    free(fresh);
    free(carried);
    free(list);
    finish_case();
}

static int run_binary_with_token(const char *list_path, program *run) {
    prepare_program(run, base_folder, "token");
    const char *words[] = {"marketplace", "build", "--list", list_path, "--out", "unused/copy.json", "--token-env", TOKEN_VARIABLE, "--token-host", "github.com", NULL};
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
    write_in(base_folder, "local.json", "{\"format\":1,\"entries\":[{\"id\":\"local:a\"}]}");
    char *list = in_base("local.json");
    program run;
    CHECK(run_binary_with_token(list, &run) == 1);
    expect_contains(run.err, "local:a is a local id", "the refusal");
    CHECK(strstr(run.err, TOKEN) == NULL && strstr(run.out, TOKEN) == NULL);
    program_free(&run);
    free(list);
    finish_case();
}

static int count_of(const char *text, const char *fragment) {
    int count = 0;
    for (const char *at = strstr(text, fragment); at != NULL; at = strstr(at + 1, fragment)) {
        count++;
    }
    return count;
}

static void run_against_the_server(void) {
    static const char runtime[] = REPOSITORY("1", "runtime", "spisor/runtime");
    static const char weivel[] = REPOSITORY("2", "weivel", "spisor/weivel");
    static const char reserved[] = REPOSITORY("6", "Con", "spisor/Con");
    static const char another_runtime[] = REPOSITORY("4", "runtime", "other/runtime");
    const http_route routes[] = {
        ROUTE("/repositories/1", 200, runtime),
        ROUTE("/repositories/2", 200, weivel),
        ROUTE("/repositories/4", 200, another_runtime),
        ROUTE("/repositories/6", 200, reserved),
    {"/repositories/70", 401, NULL, "", 0, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0},
    {"/repositories/60", 404, NULL, "", 0, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0},
    };
    http_fixture *server = http_fixture_start(routes, sizeof routes / sizeof routes[0]);
    CHECK(server != NULL);
    if (server == NULL) {
        return;
    }
    char api_base[128];
    snprintf(api_base, sizeof api_base, "http://127.0.0.1:%u", (unsigned)http_fixture_port(server));
    host_marketplace_aim_lookups(api_base);
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
    host_marketplace_aim_lookups(NULL);
    char *requests = http_fixture_stop(server);
    CHECK(requests != NULL && count_of(requests, AUTHORIZATION_LINE) == 2);
    CHECK(requests != NULL && count_of(requests, "uthorization") == 2);
    free(requests);
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
    answers_2_when_the_list_or_the_out_is_missing();
    prints_a_name_with_its_control_characters_escaped();
    the_binary_refuses_a_local_id_and_escapes_it_on_the_terminal();
    the_binary_never_prints_the_token();
    return check_summary("marketplace_command_test");
}
