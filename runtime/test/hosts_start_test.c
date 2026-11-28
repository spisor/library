#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "hosts_call.h"
#include "spisor/allocation.h"
#include "updates_fixture.h"

#include "host_references.h"
#include "hosts_start.h"
#include "platform_key.h"
#include "run_records.h"

#define TEST_HOST "test-host"
#define LAYER "typescript-host"
#define TYPESCRIPT "typescript"
#define FIXTURE "fixture-one"
#define RECORDED_RUN                                                                                                \
    "{\"test-host\":\"1.0.0\",\"typescript-host\":\"2.0.0\",\"runtime\":\"0.1.0\",\"a\":\"0.9.0\"}"

static int record_out_of_memory;

static char *written_run_record(const char *at, const char *host, const cJSON *run, double check_milliseconds) {
    spisor_prepared_path removal;
    char *record = runtime_record_run(at, host, run, check_milliseconds, &removal, &record_out_of_memory);
    if (record != NULL) {
        spisor_release_prepared_path(&removal);
    }
    return record;
}

static char user_home[1400];
static char application_home[1400];
static char portable[1400];

/** Lists `<language>-host` as a language-host item started under `runtime`, at 2.0.0. */
static void list_language_host(const char *language, const char *runtime) {
    char id[128];
    snprintf(id, sizeof id, "%s-host", language);
    cJSON *item = cJSON_AddObjectToObject(catalog_items, id);
    cJSON_AddStringToObject(item, "kind", "language-host");
    cJSON_AddStringToObject(item, "language", language);
    cJSON_AddStringToObject(item, "runtime", runtime);
    cJSON_AddStringToObject(item, "latest", "2.0.0");
    cJSON_AddObjectToObject(cJSON_AddObjectToObject(cJSON_AddObjectToObject(item, "versions"), "2.0.0"), "files");
}

/** A world whose catalog hosts TypeScript on javascript and the fixture language on native, and lists plugin a. */
static void start_world(const char *label) {
    start_base(label);
    snprintf(user_home, sizeof user_home, "%s/user", scratch);
    snprintf(application_home, sizeof application_home, "%s/application", scratch);
    snprintf(portable, sizeof portable, "%s/portable", scratch);
    list_language_host("typescript", "javascript");
    list_language_host("fixture-one", "native");
    list_plugin("a", "1.0.0", "1.0.0");
    write_catalog();
    mark_installed("bundles", "a", "1.0.0");
}

static void write_tree(const char *base, const char *rest, cJSON *tree) {
    char *text = cJSON_PrintUnformatted(tree);
    write_in(base, rest, text);
    cJSON_free(text);
    cJSON_Delete(tree);
}

static void register_test_host(const char *version) {
    cJSON *registration = cJSON_CreateObject();
    cJSON_AddStringToObject(registration, "id", TEST_HOST);
    cJSON_AddStringToObject(registration, "kind", "host");
    if (version != NULL) {
        cJSON_AddStringToObject(registration, "version", version);
    }
    cJSON_AddStringToObject(registration, "home", application_home);
    write_tree(state, "hosts/" TEST_HOST ".json", registration);
}

/** The loader's standard at its registered version, requiring `plugin` first, so it holds the front door. */
static void require_front_door(const char *plugin) {
    char standard[256];
    snprintf(standard, sizeof standard, "{\"plugins\":[\"%s\"]}", plugin);
    write_in(cache, "bundles/" TEST_HOST "/1.0.0/host.json", standard);
}

static void write_settings(const char *home, const char *extra_json) {
    write_tree(home, "config/settings.json", settings_with(extra_json));
}

static void enable_plugin_a(void) {
    write_settings(application_home, "{\"plugins\":{\"a\":{\"enabled\":true}}}");
}

static cJSON *environment_json(void) {
    cJSON *environment = cJSON_CreateObject();
    cJSON_AddStringToObject(environment, "HOME", user_home);
    cJSON_AddStringToObject(environment, "USERPROFILE", user_home);
    cJSON_AddStringToObject(environment, "SPISOR_HOME", portable);
    cJSON_AddStringToObject(environment, "SPISOR_STATE", state);
    cJSON_AddStringToObject(environment, "SPISOR_CACHE", cache);
    return environment;
}

/** A start of TEST_HOST's `layer_language` layer, and of an action of `action_plugin` when it is not NULL. */
static catalog_answer start_layer_in(cJSON *environment, int session, const char *layer_language,
                                     const char *action_plugin) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "environment", environment);
    cJSON_AddStringToObject(argument, "hostId", TEST_HOST);
    cJSON_AddItemToObject(argument, "layer", parsed("{\"id\":\"" LAYER "\",\"version\":\"2.0.0\"}"));
    cJSON_AddItemToObject(argument, "libraries", parsed("{\"runtime\":\"0.1.0\"}"));
    cJSON_AddBoolToObject(argument, "session", session);
    cJSON_AddStringToObject(argument, "layerLanguage", layer_language);
    if (action_plugin != NULL) {
        cJSON_AddStringToObject(argument, "actionPlugin", action_plugin);
    }
    return hosts_call(HOSTS_START, argument);
}

static catalog_answer start_host_in(cJSON *environment, int session) {
    return start_layer_in(environment, session, TYPESCRIPT, NULL);
}

static catalog_answer start_host(int session) {
    return start_host_in(environment_json(), session);
}

static int is_true_at(const cJSON *tree, const char *slashed) {
    return cJSON_IsTrue(value_at(tree, slashed));
}

static int is_false_at(const cJSON *tree, const char *slashed) {
    return cJSON_IsFalse(value_at(tree, slashed));
}

static void record_a_previous_start(void) {
    record_references(TEST_HOST, "{\"run\":" RECORDED_RUN ",\"next\":{}}");
}

static char *record_a_live_run(void) {
    cJSON *run = parsed(RECORDED_RUN);
    char *record = written_run_record(state, TEST_HOST, run, RUNTIME_DEFAULT_CHECK_MILLISECONDS);
    cJSON_Delete(run);
    CHECK(record != NULL);
    return record;
}

static void record_a_dead_run(void) {
    char content[512];
    snprintf(content, sizeof content, "{\"pid\":%d,\"aliveAt\":1,\"checkMilliseconds\":5000,\"run\":" RECORDED_RUN "}\n",
             (int)current_process_id());
    write_in(state, "bundles/runs/" TEST_HOST "/stopped.json", content);
}

static void a_session_start_applies_updates(void) {
    start_world("start-session");
    register_test_host("1.0.0");
    enable_plugin_a();
    record_a_previous_start();
    spisor_memory_release(record_a_live_run());
    catalog_answer started = start_host(1);
    expect_success(started, "session start");
    CHECK(is_false_at(started.tree, "reusedRecordedRun"));
    expect_member_json(started.tree, "plan/plugins", "{\"a\":\"1.0.0\"}", "session plugins");
    CHECK(cJSON_IsObject(value_at(started.tree, "plan/next")));
    CHECK(cJSON_IsArray(value_at(started.tree, "plan/unrecorded")));
    CHECK(text_at(value_at(started.tree, "plan"), "runRecord") != NULL);
    expect_member_json(started.tree, "plan/run",
                       "{\"test-host\":\"1.0.0\",\"typescript-host\":\"2.0.0\",\"runtime\":\"0.1.0\",\"a\":\"1.0.0\"}",
                       "session run");
    char *identity = NULL;
    cJSON *expected_identity = cJSON_CreateObject();
    cJSON_AddStringToObject(expected_identity, "version", "1.0.0");
    cJSON_AddStringToObject(expected_identity, "home", application_home);
    cJSON_AddBoolToObject(expected_identity, "requiresCommand", 0);
    identity = cJSON_PrintUnformatted(expected_identity);
    expect_member_json(started.tree, "identity", identity, "identity");
    cJSON_free(identity);
    cJSON_Delete(expected_identity);
    check_text(text_at(value_at(started.tree, "places"), "home"), user_home, "user home");
    check_text(text_at(value_at(started.tree, "places"), "state"), state, "places state");
    check_text(text_at(value_at(started.tree, "places"), "cache"), cache, "places cache");
    check_text(text_at(value_at(started.tree, "places"), "portable"), portable, "places portable");
    cJSON_Delete(started.tree);
    finish_base();
}

static void a_non_session_start_reuses_a_live_recorded_run(void) {
    start_world("start-reuse");
    register_test_host("1.0.0");
    enable_plugin_a();
    record_a_previous_start();
    char *live = record_a_live_run();
    catalog_answer started = start_host(0);
    expect_success(started, "reuse");
    CHECK(is_true_at(started.tree, "reusedRecordedRun"));
    expect_member_json(started.tree, "plan/plugins", "{\"a\":\"0.9.0\"}", "reused plugins");
    expect_member_json(started.tree, "plan/run", RECORDED_RUN, "reused run");
    expect_member_json(started.tree, "plan/unrecorded", "[]", "reused unrecorded");
    const char *record = text_at(value_at(started.tree, "plan"), "runRecord");
    CHECK(record != NULL && spisor_probe_file(record) == SPISOR_FILE_PRESENT);
    CHECK(record != NULL && live != NULL && strcmp(record, live) != 0);
    cJSON *stamped = record == NULL ? NULL : read_json_file(record);
    expect_member_json(stamped, "run", RECORDED_RUN, "stamped run");
    CHECK(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(stamped, "pid")) == (double)current_process_id());
    cJSON_Delete(stamped);
    expect_references(TEST_HOST, "{\"run\":" RECORDED_RUN ",\"next\":{}}", "references untouched");
    spisor_memory_release(live);
    cJSON_Delete(started.tree);
    finish_base();
}

static void a_non_session_start_plans_afresh_when_the_recorded_run_is_dead(void) {
    start_world("start-dead");
    register_test_host("1.0.0");
    enable_plugin_a();
    record_a_previous_start();
    record_a_dead_run();
    catalog_answer started = start_host(0);
    expect_success(started, "dead run");
    CHECK(is_false_at(started.tree, "reusedRecordedRun"));
    expect_member_json(started.tree, "plan/plugins", "{\"a\":\"1.0.0\"}", "planned afresh");
    cJSON_Delete(started.tree);
    finish_base();
}

static void a_live_recorded_run_with_a_version_that_is_no_text_is_refused_as_unreadable(void) {
    start_world("start-run-number");
    register_test_host("1.0.0");
    enable_plugin_a();
    record_references(TEST_HOST, "{\"run\":{\"test-host\":\"1.0.0\",\"a\":1},\"next\":{}}");
    spisor_memory_release(record_a_live_run());
    catalog_answer started = start_host(0);
    CHECK(started.status == SPISOR_REFUSED);
    const char *message = text_at(started.tree, "message");
    CHECK(message != NULL && strstr(message, "could not be read, so what its last start ran is unknown") != NULL);
    cJSON_Delete(started.tree);
    finish_base();
}

static void the_running_map_names_the_host_the_layer_and_every_library(void) {
    start_world("start-running");
    register_test_host("1.0.0");
    enable_plugin_a();
    catalog_answer started = start_host(1);
    expect_success(started, "running");
    expect_member_json(started.tree, "plan/plugins", "{\"a\":\"1.0.0\"}", "running excluded from plugins");
    expect_references(TEST_HOST,
                      "{\"run\":{\"test-host\":\"1.0.0\",\"typescript-host\":\"2.0.0\",\"runtime\":\"0.1.0\","
                      "\"a\":\"1.0.0\"},\"next\":{}}",
                      "running recorded");
    cJSON_Delete(started.tree);
    finish_base();
}

static void expect_not_registered(catalog_answer answer, const char *label) {
    char message[2000];
    snprintf(message, sizeof message, "the host " TEST_HOST " is not registered in %s", state);
    CHECK(answer.status == SPISOR_REFUSED);
    check_text(text_at(answer.tree, "message"), message, label);
    expect_fix(answer, REINSTALL_FIX, label);
}

static void an_unregistered_host_is_refused_with_the_reinstall_fix(void) {
    start_world("start-unregistered");
    catalog_answer started = start_host(1);
    expect_not_registered(started, "unregistered");
    cJSON_Delete(started.tree);
    finish_base();
}

static void a_registration_without_a_version_is_refused(void) {
    start_world("start-versionless");
    register_test_host(NULL);
    catalog_answer started = start_host(1);
    expect_not_registered(started, "versionless");
    cJSON_Delete(started.tree);
    finish_base();
}

static void the_settings_come_from_the_registrations_home(void) {
    start_world("start-settings");
    register_test_host("1.0.0");
    write_settings(application_home, "{\"marker\":\"application\"}");
    write_settings(user_home, "{\"marker\":\"user\"}");
    catalog_answer started = start_host(1);
    expect_success(started, "settings");
    check_text(text_at(value_at(started.tree, "settings"), "marker"), "application", "settings home");
    cJSON_Delete(started.tree);
    finish_base();
}

/** no-color.org's rule: NO_COLOR counts only when it is set to a value that is not empty. */
static void no_color_counts_only_when_set_and_not_empty(void) {
    start_world("start-no-color");
    register_test_host("1.0.0");
    enable_plugin_a();
    cJSON *empty = environment_json();
    cJSON_AddStringToObject(empty, "NO_COLOR", "");
    catalog_answer emptied = start_host_in(empty, 1);
    expect_success(emptied, "an empty no color");
    CHECK(is_false_at(emptied.tree, "noColor"));
    cJSON *set = environment_json();
    cJSON_AddStringToObject(set, "NO_COLOR", "1");
    catalog_answer colorless = start_host_in(set, 1);
    expect_success(colorless, "no color");
    CHECK(is_true_at(colorless.tree, "noColor"));
    catalog_answer colored = start_host(1);
    CHECK(is_false_at(colored.tree, "noColor"));
    cJSON_Delete(colored.tree);
    cJSON_Delete(colorless.tree);
    cJSON_Delete(emptied.tree);
    finish_base();
}

/** An environment is the OS's, every value text, so a NO_COLOR that is no text is a malformed call, never colour. */
static void a_no_color_that_is_no_text_is_refused_as_no_usable_environment(void) {
    start_world("start-no-color-number");
    register_test_host("1.0.0");
    enable_plugin_a();
    cJSON *environment = environment_json();
    cJSON_AddNumberToObject(environment, "NO_COLOR", 1);
    cJSON *libraries = parsed("{\"runtime\":\"0.1.0\"}");
    runtime_hosts_start_request request = {environment, TEST_HOST, LAYER, "2.0.0", libraries, 1, 1, TYPESCRIPT, NULL};
    cJSON *refusal = NULL;
    cJSON *answer = runtime_hosts_start(&request, &refusal);
    CHECK(answer == NULL);
    check_text(text_at(refusal, "message"), "the call carries no usable environment", "a number for NO_COLOR");
    cJSON_Delete(answer);
    cJSON_Delete(refusal);
    cJSON_Delete(libraries);
    cJSON_Delete(environment);
    finish_base();
}

static void no_color_ignores_case_on_windows(void) {
    start_world("start-no-color-case");
    register_test_host("1.0.0");
    enable_plugin_a();
    cJSON *environment = environment_json();
    cJSON_AddStringToObject(environment, "no_color", "1");
    catalog_answer started = start_host_in(environment, 1);
    expect_success(started, "lowercase no color");
    CHECK(cJSON_IsTrue(value_at(started.tree, "noColor")) == RUNTIME_HOST_IS_WINDOWS);
    cJSON_Delete(started.tree);
    finish_base();
}

static void a_recorded_plugin_differing_only_in_case_from_a_running_id_is_kept(void) {
    start_world("start-plugin-case");
    register_test_host("1.0.0");
    enable_plugin_a();
    cJSON *run = parsed(RECORDED_RUN);
    cJSON_AddStringToObject(run, "Runtime", "3.0.0");
    char *text = cJSON_PrintUnformatted(run);
    char references[1024];
    snprintf(references, sizeof references, "{\"run\":%s,\"next\":{}}", text);
    record_references(TEST_HOST, references);
    spisor_memory_release(written_run_record(state, TEST_HOST, run, RUNTIME_DEFAULT_CHECK_MILLISECONDS));
    catalog_answer started = start_host(0);
    expect_success(started, "plugin case");
    check_text(text_at(value_at(started.tree, "plan/run"), "Runtime"), "3.0.0", "the recorded id in its own case kept");
    check_text(text_at(value_at(started.tree, "plan/run"), "runtime"), "0.1.0", "the running library beside it");
    cJSON_free(text);
    cJSON_Delete(run);
    cJSON_Delete(started.tree);
    finish_base();
}

static void a_live_run_naming_the_host_in_another_case_does_not_keep_its_recorded_run(void) {
    start_world("start-host-case");
    register_test_host("1.0.0");
    enable_plugin_a();
    record_a_previous_start();
    cJSON *other = parsed("{\"Test-Host\":\"1.0.0\"}");
    spisor_memory_release(written_run_record(state, "other-host", other, RUNTIME_DEFAULT_CHECK_MILLISECONDS));
    cJSON_Delete(other);
    catalog_answer started = start_host(0);
    expect_success(started, "host case");
    CHECK(is_false_at(started.tree, "reusedRecordedRun"));
    cJSON_Delete(started.tree);
    finish_base();
}

static void the_timers_are_the_runtimes_own(void) {
    start_world("start-timers");
    register_test_host("1.0.0");
    enable_plugin_a();
    catalog_answer started = start_host(1);
    expect_success(started, "timers");
    expect_member_json(started.tree, "timers",
                       "{\"recordRunMilliseconds\":5000,\"pruneFailuresBeforeExit\":6,\"updateCheckMilliseconds\":3600000}",
                       "timers");
    cJSON_Delete(started.tree);
    finish_base();
}

static void a_non_session_start_answers_no_timers(void) {
    start_world("start-no-timers");
    register_test_host("1.0.0");
    enable_plugin_a();
    catalog_answer started = start_host(0);
    expect_success(started, "no timers");
    CHECK(value_at(started.tree, "timers") == NULL);
    CHECK(value_at(started.tree, "home") == NULL);
    cJSON_Delete(started.tree);
    finish_base();
}

/** A library version installed for the platform this build of the runtime resolves for. */
static void install_library_here(const char *id, const char *version) {
    char rest[512];
    snprintf(rest, sizeof rest, "libraries/%s/%s/%s/.installed.json", id, version, runtime_platform_key());
    list_library(id, version, version);
    write_in(cache, rest, "{\"files\":{}}");
}

static void a_start_writes_no_hint_for_the_registered_host(void) {
    start_world("start-no-hint");
    install_library_here("runtime", "0.1.0");
    install_library_here("runtime", "0.2.0");
    write_catalog();
    register_test_host("1.0.0");
    enable_plugin_a();
    catalog_answer started = start_host(1);
    expect_success(started, "no hint");
    CHECK(!exists_in(cache, "hosts/" TEST_HOST ".json"));
    cJSON_Delete(started.tree);
    finish_base();
}

static void the_identity_version_is_the_one_the_plan_runs(void) {
    start_world("start-identity-version");
    register_test_host("1.0.0");
    enable_plugin_a();
    record_references(TEST_HOST, "{\"run\":{\"" TEST_HOST "\":\"1.0.0\"},\"next\":{\"" TEST_HOST "\":\"2.0.0\"}}");
    mark_installed("bundles", TEST_HOST, "2.0.0");
    catalog_answer started = start_host(1);
    expect_success(started, "identity version");
    check_text(text_at(value_at(started.tree, "plan/run"), TEST_HOST), "2.0.0", "switched run");
    check_text(text_at(value_at(started.tree, "identity"), "version"), "2.0.0", "identity runs the switched version");
    cJSON_Delete(started.tree);
    finish_base();
}

static cJSON *start_in_process(void) {
    cJSON *environment = environment_json();
    cJSON *libraries = parsed("{\"runtime\":\"0.1.0\"}");
    runtime_hosts_start_request request = {environment, TEST_HOST, LAYER, "2.0.0", libraries, 1, 1, TYPESCRIPT, NULL};
    cJSON *refusal = NULL;
    cJSON *answer = runtime_hosts_start(&request, &refusal);
    CHECK(answer != NULL && refusal == NULL);
    cJSON_Delete(refusal);
    cJSON_Delete(libraries);
    cJSON_Delete(environment);
    return answer;
}

/**
 * A second in-process host in one home, as a second OpenCode starts, reuses the run the first recorded while the
 * first's run record is live, and leaves a version scheduled since then for the next session start to apply.
 */
static void a_second_in_process_start_reuses_the_live_run_beside_a_scheduled_version(void) {
    start_world("start-in-process-twice");
    register_test_host("1.0.0");
    enable_plugin_a();
    record_references(TEST_HOST, "{\"run\":{\"" TEST_HOST "\":\"1.0.0\"},\"next\":{}}");
    mark_installed("bundles", TEST_HOST, "2.0.0");
    cJSON *first = start_in_process();
    CHECK(cJSON_IsFalse(value_at(first, "reusedRecordedRun")));
    check_text(text_at(value_at(first, "plan/run"), TEST_HOST), "1.0.0", "the first runs the recorded version");
    cJSON *scheduled = cJSON_CreateObject();
    cJSON_AddItemToObject(scheduled, "run", cJSON_Duplicate(value_at(first, "plan/run"), 1));
    cJSON_AddItemToObject(scheduled, "next", parsed("{\"" TEST_HOST "\":\"2.0.0\"}"));
    char *references = cJSON_PrintUnformatted(scheduled);
    record_references(TEST_HOST, references);
    cJSON_free(references);
    cJSON_Delete(scheduled);
    cJSON *second = start_in_process();
    CHECK(cJSON_IsTrue(value_at(second, "reusedRecordedRun")));
    check_text(text_at(value_at(second, "plan/run"), TEST_HOST), "1.0.0", "the second reuses the running version");
    runtime_host_references recorded = runtime_read_host_references(state, TEST_HOST);
    expect_json(recorded.next, "{\"" TEST_HOST "\":\"2.0.0\"}", "the scheduled version left for a session start");
    runtime_free_host_references(&recorded);
    cJSON_Delete(first);
    cJSON_Delete(second);
    finish_base();
}

static void a_refused_session_start_publishes_its_refusal(void) {
    start_world("start-refusal-published");
    catalog_answer started = start_host(1);
    CHECK(started.status == SPISOR_REFUSED);
    char *path = joined(state, "processes/" TEST_HOST ".ready.json");
    cJSON *record = read_json_file(path);
    CHECK(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(record, "pid")) == (double)current_process_id());
    check_text(text_at(record, "refused"), text_at(started.tree, "message"), "published refusal");
    check_text(text_at(record, "fix"), REINSTALL_FIX, "published fix");
    cJSON_Delete(record);
    spisor_memory_release(path);
    cJSON_Delete(started.tree);
    finish_base();
}

static void a_refused_non_session_start_publishes_nothing(void) {
    start_world("start-refusal-unpublished");
    catalog_answer started = start_host(0);
    CHECK(started.status == SPISOR_REFUSED);
    CHECK(!exists_in(state, "processes/" TEST_HOST ".ready.json"));
    cJSON_Delete(started.tree);
    finish_base();
}

/**
 * Lists plugin `id` at 1.0.0 written in `language` and installs it with a manifest declaring `capabilities` and,
 * unless NULL, `actions`, both JSON objects; the catalog is written by the caller.
 */
static void install_layered_plugin(const char *id, const char *language, const char *capabilities, const char *actions) {
    char manifest[1024];
    char rest[512];
    list_plugin(id, "1.0.0", "1.0.0");
    cJSON *item = cJSON_GetObjectItemCaseSensitive(catalog_items, id);
    cJSON_ReplaceItemInObjectCaseSensitive(item, "language", cJSON_CreateString(language));
    mark_installed("bundles", id, "1.0.0");
    snprintf(manifest, sizeof manifest, "{\"id\":\"%s\",\"version\":\"1.0.0\",\"capabilities\":%s%s%s}", id,
             capabilities, actions == NULL ? "" : ",\"actions\":", actions == NULL ? "" : actions);
    snprintf(rest, sizeof rest, "bundles/%s/1.0.0/plugin.json", id);
    write_in(cache, rest, manifest);
}

#define NO_CAPABILITIES "{\"provides\":[],\"consumes\":[]}"
#define GREET_ACTION "{\"greet\":{\"call\":{\"service\":\"plugin-stub\",\"method\":\"greet\"}}}"
#define LOGIN_ACTION "{\"login\":{\"call\":{\"service\":\"provider:accounts\",\"method\":\"login\"}}}"

/** plugin-stub with an action, a SERVE-only proxy consuming forebay.provider, and its provider, all TypeScript. */
static void lay_out_proxy_world(const char *label) {
    start_world(label);
    register_test_host("1.0.0");
    install_layered_plugin("plugin-stub", TYPESCRIPT, NO_CAPABILITIES, GREET_ACTION);
    install_layered_plugin("proxy", TYPESCRIPT, "{\"provides\":[],\"consumes\":[\"forebay.provider\"]}", NULL);
    install_layered_plugin("provider", TYPESCRIPT, "{\"provides\":[\"forebay.provider\"],\"consumes\":[]}",
                           LOGIN_ACTION);
    write_catalog();
    write_settings(application_home, "{\"plugins\":{\"plugin-stub\":{\"enabled\":true},\"proxy\":{\"enabled\":true},"
                                     "\"provider\":{\"enabled\":true}}}");
}

static void lay_out_two_layers(const char *label) {
    start_world(label);
    register_test_host("1.0.0");
    require_front_door("scripted");
    install_layered_plugin("scripted", TYPESCRIPT, NO_CAPABILITIES, NULL);
    install_layered_plugin("compiled", FIXTURE, NO_CAPABILITIES, NULL);
    write_catalog();
    write_settings(application_home, "{\"plugins\":{\"scripted\":{\"enabled\":true},\"compiled\":{\"enabled\":true}}}");
}

static void a_run_identity_may_carry_its_layer_after_a_dot(void) {
    CHECK(runtime_is_plain_host_id("claude-code-loader.fixture-one"));
}

static void each_layer_plans_only_the_plugins_of_its_language(void) {
    lay_out_two_layers("start-layers");
    catalog_answer javascript = start_layer_in(environment_json(), 1, TYPESCRIPT, NULL);
    expect_success(javascript, "javascript layer");
    expect_member_json(javascript.tree, "plan/plugins", "{\"scripted\":\"1.0.0\"}", "the javascript layer's plugins");
    catalog_answer native = start_layer_in(environment_json(), 1, FIXTURE, NULL);
    expect_success(native, "native layer");
    expect_member_json(native.tree, "plan/plugins", "{\"compiled\":\"1.0.0\"}", "the native layer's plugins");
    cJSON_Delete(javascript.tree);
    cJSON_Delete(native.tree);
    finish_base();
}

static void a_consumer_whose_only_provider_runs_in_the_other_layer_is_refused(void) {
    start_world("start-split");
    register_test_host("1.0.0");
    require_front_door("consumer");
    install_layered_plugin("consumer", TYPESCRIPT, "{\"provides\":[],\"consumes\":[\"forebay.provider\"]}", NULL);
    install_layered_plugin("compiled-provider", FIXTURE, "{\"provides\":[\"forebay.provider\"],\"consumes\":[]}", NULL);
    write_catalog();
    write_settings(application_home,
                   "{\"plugins\":{\"consumer\":{\"enabled\":true},\"compiled-provider\":{\"enabled\":true}}}");
    catalog_answer started = start_layer_in(environment_json(), 1, TYPESCRIPT, NULL);
    CHECK(started.status == SPISOR_REFUSED);
    const char *message = text_at(started.tree, "message");
    CHECK(message != NULL && strstr(message, "consumer (typescript)") != NULL);
    CHECK(message != NULL && strstr(message, "compiled-provider (fixture-one)") != NULL);
    expect_fix(started, "enable a provider of forebay.provider written in the consumer's language, or disable consumer",
               "the split fix");
    char *path = joined(state, "processes/" TEST_HOST ".ready.json");
    cJSON *record = read_json_file(path);
    check_text(text_at(record, "refused"), message, "the split refusal published");
    cJSON_Delete(record);
    spisor_memory_release(path);
    cJSON_Delete(started.tree);
    finish_base();
}

static void expect_refused_with(catalog_answer started, const char *message_part, const char *fix, const char *label) {
    const char *message = text_at(started.tree, "message");
    if (started.status != SPISOR_REFUSED || message == NULL || strstr(message, message_part) == NULL) {
        fprintf(stderr, "%s: expected a refusal naming \"%s\", got status %d and \"%s\"\n", label, message_part,
                (int)started.status, message == NULL ? "(none)" : message);
        check_failures++;
    }
    expect_fix(started, fix, label);
}

static void a_start_no_language_can_hold_the_front_door_of_is_refused(void) {
    start_world("start-two-doors");
    register_test_host("1.0.0");
    install_layered_plugin("scripted", TYPESCRIPT, NO_CAPABILITIES, NULL);
    install_layered_plugin("compiled", FIXTURE, NO_CAPABILITIES, NULL);
    write_catalog();
    write_settings(application_home, "{\"plugins\":{\"scripted\":{\"enabled\":true},\"compiled\":{\"enabled\":true}}}");
    catalog_answer started = start_layer_in(environment_json(), 1, TYPESCRIPT, NULL);
    expect_refused_with(started, "so no one language holds its front door",
                        "require the plugin that holds the front door in the loader's standard, or enable plugins of "
                        "one language only, then install the loader again",
                        "two languages and no required plugin");
    cJSON_Delete(started.tree);
    finish_base();
}

static void a_start_whose_first_required_plugin_no_catalog_lists_is_refused(void) {
    start_world("start-unlisted-door");
    register_test_host("1.0.0");
    require_front_door("brewed");
    install_layered_plugin("scripted", TYPESCRIPT, NO_CAPABILITIES, NULL);
    write_catalog();
    write_settings(application_home, "{\"plugins\":{\"scripted\":{\"enabled\":true}}}");
    catalog_answer started = start_layer_in(environment_json(), 1, TYPESCRIPT, NULL);
    expect_refused_with(started, "brewed, the first plugin the loader requires, is listed in no catalog source",
                        "add a catalog source listing brewed, then install the loader again", "the unlisted front door");
    cJSON_Delete(started.tree);
    finish_base();
}

static void an_unknown_layer_language_is_malformed(void) {
    start_world("start-unknown-layer");
    register_test_host("1.0.0");
    catalog_answer started = start_layer_in(environment_json(), 1, "spisor", NULL);
    CHECK(started.status == SPISOR_MALFORMED);
    cJSON_Delete(started.tree);
    finish_base();
}

/** An action of plugin-stub beside a live session's SERVE-only proxy must never start a second proxy. */
static void an_action_start_plans_the_actions_plugin_without_a_serve_only_plugin_beside_it(void) {
    lay_out_proxy_world("start-action-alone");
    catalog_answer started = start_layer_in(environment_json(), 0, TYPESCRIPT, "plugin-stub");
    expect_success(started, "action alone");
    expect_member_json(started.tree, "plan/plugins", "{\"plugin-stub\":\"1.0.0\"}", "the action's plugin alone");
    cJSON_Delete(started.tree);
    finish_base();
}

static void an_action_start_of_a_consumer_plans_its_provider_too(void) {
    lay_out_proxy_world("start-action-provider");
    catalog_answer started = start_layer_in(environment_json(), 0, TYPESCRIPT, "proxy");
    expect_success(started, "action with provider");
    expect_member_json(started.tree, "plan/plugins", "{\"provider\":\"1.0.0\",\"proxy\":\"1.0.0\"}",
                       "the consumer and its provider");
    cJSON_Delete(started.tree);
    finish_base();
}

/** `front`'s action consumes x, `middle` provides x and consumes y, `back` provides y; `watcher` consumes y alone. */
static void an_action_start_plans_the_providers_of_its_providers(void) {
    start_world("start-action-transitive");
    register_test_host("1.0.0");
    install_layered_plugin("front", TYPESCRIPT, "{\"provides\":[],\"consumes\":[\"x\"]}", GREET_ACTION);
    install_layered_plugin("middle", TYPESCRIPT, "{\"provides\":[\"x\"],\"consumes\":[\"y\"]}", NULL);
    install_layered_plugin("back", TYPESCRIPT, "{\"provides\":[\"y\"],\"consumes\":[]}", NULL);
    install_layered_plugin("watcher", TYPESCRIPT, "{\"provides\":[],\"consumes\":[\"y\"]}", NULL);
    write_catalog();
    write_settings(application_home, "{\"plugins\":{\"front\":{\"enabled\":true},\"middle\":{\"enabled\":true},"
                                     "\"back\":{\"enabled\":true},\"watcher\":{\"enabled\":true}}}");
    catalog_answer started = start_layer_in(environment_json(), 0, TYPESCRIPT, "front");
    expect_success(started, "transitive action");
    expect_member_json(started.tree, "plan/plugins",
                       "{\"back\":\"1.0.0\",\"front\":\"1.0.0\",\"middle\":\"1.0.0\"}",
                       "the action's plugin, its provider and its provider's provider");
    cJSON_Delete(started.tree);
    finish_base();
}

static cJSON *start_layer_directly(const cJSON *environment, const cJSON *libraries, const char *layer_id,
                                   const char *layer_language) {
    runtime_hosts_start_request request = {environment, TEST_HOST, layer_id, "2.0.0", libraries, 0, 0,
                                           layer_language, NULL};
    cJSON *refusal = NULL;
    cJSON *answer = runtime_hosts_start(&request, &refusal);
    CHECK(answer != NULL && refusal == NULL);
    cJSON_Delete(refusal);
    return answer;
}

/** The layer that does not hold the front door runs as `<host>.<runtime>`, the one that does as the host. */
static void the_layer_without_the_front_door_runs_under_its_own_identity(void) {
    lay_out_two_layers("start-identities");
    cJSON *libraries = parsed("{\"runtime\":\"0.1.0\"}");
    cJSON *environment = environment_json();
    cJSON *front = start_layer_directly(environment, libraries, LAYER, TYPESCRIPT);
    cJSON *other = start_layer_directly(environment, libraries, "fixture-one-host", FIXTURE);
    check_text(text_at(front, "runId"), TEST_HOST, "the front door's identity");
    check_text(text_at(other, "runId"), TEST_HOST "." FIXTURE, "the other layer's identity");
    runtime_hosts_start_undo(cJSON_GetObjectItemCaseSensitive(front, "plan"));
    runtime_hosts_start_undo(cJSON_GetObjectItemCaseSensitive(other, "plan"));
    cJSON_Delete(front);
    cJSON_Delete(other);
    cJSON_Delete(environment);
    cJSON_Delete(libraries);
    finish_base();
}

/** Two layers each checking for updates would schedule and fetch the same versions into one cache at once. */
static void only_the_front_door_layer_checks_for_updates(void) {
    lay_out_two_layers("start-update-check");
    catalog_answer front = start_layer_in(environment_json(), 1, TYPESCRIPT, NULL);
    catalog_answer other = start_layer_in(environment_json(), 1, FIXTURE, NULL);
    expect_success(front, "the front door's session start");
    expect_success(other, "the other layer's session start");
    CHECK(cJSON_GetNumberValue(value_at(front.tree, "timers/updateCheckMilliseconds")) ==
          RUNTIME_UPDATE_CHECK_MILLISECONDS);
    CHECK(cJSON_GetNumberValue(value_at(other.tree, "timers/updateCheckMilliseconds")) == 0);
    cJSON_Delete(front.tree);
    cJSON_Delete(other.tree);
    finish_base();
}

static void an_action_of_a_plugin_in_the_other_layer_is_refused_naming_its_layer(void) {
    start_world("start-action-other-layer");
    register_test_host("1.0.0");
    require_front_door("scripted");
    install_layered_plugin("scripted", TYPESCRIPT, NO_CAPABILITIES, NULL);
    install_layered_plugin("compiled", FIXTURE, NO_CAPABILITIES, GREET_ACTION);
    write_catalog();
    write_settings(application_home, "{\"plugins\":{\"scripted\":{\"enabled\":true},\"compiled\":{\"enabled\":true}}}");
    catalog_answer started = start_layer_in(environment_json(), 0, TYPESCRIPT, "compiled");
    CHECK(started.status == SPISOR_REFUSED);
    const char *message = text_at(started.tree, "message");
    CHECK(message != NULL && strstr(message, "compiled runs in the fixture-one layer") != NULL);
    expect_fix(started, "start the action in the fixture-one layer, as spisor-host action does", "the misplaced fix");
    cJSON_Delete(started.tree);
    finish_base();
}

/**
 * The other layer's session start reuses the run the front door's applied: applying again would record a run naming
 * its own layer alone and an empty rollback over the front door's.
 */
static void one_session_applies_once_and_the_other_layer_reuses_its_run(void) {
    lay_out_two_layers("start-one-apply");
    record_references(TEST_HOST, "{\"run\":{\"" TEST_HOST "\":\"1.0.0\"},\"next\":{\"" TEST_HOST "\":\"2.0.0\"}}");
    mark_installed("bundles", TEST_HOST, "2.0.0");
    catalog_answer front = start_layer_in(environment_json(), 1, TYPESCRIPT, NULL);
    expect_success(front, "the front door's session start");
    CHECK(is_false_at(front.tree, "reusedRecordedRun"));
    cJSON *libraries = parsed("{\"runtime\":\"0.1.0\"}");
    cJSON *environment = environment_json();
    runtime_hosts_start_request request = {environment, TEST_HOST, "fixture-one-host", "3.0.0", libraries, 1, 0,
                                           FIXTURE, NULL};
    cJSON *refusal = NULL;
    cJSON *other = runtime_hosts_start(&request, &refusal);
    CHECK(other != NULL && refusal == NULL);
    CHECK(cJSON_IsTrue(value_at(other, "reusedRecordedRun")));
    check_text(text_at(value_at(other, "plan/run"), "typescript-host"), "2.0.0", "the front door's layer item");
    check_text(text_at(value_at(other, "plan/run"), "fixture-one-host"), "3.0.0", "its own layer item");
    expect_member_json(other, "plan/plugins", "{\"compiled\":\"1.0.0\"}", "its own layer's plugins");
    runtime_host_references recorded = runtime_read_host_references(state, TEST_HOST);
    expect_json(recorded.rollback, "{\"" TEST_HOST "\":\"1.0.0\"}", "the front door's rollback kept");
    check_text(text_at(recorded.run, "typescript-host"), "2.0.0", "the front door's run kept");
    runtime_free_host_references(&recorded);
    runtime_hosts_start_undo(cJSON_GetObjectItemCaseSensitive(other, "plan"));
    cJSON_Delete(refusal);
    cJSON_Delete(other);
    cJSON_Delete(environment);
    cJSON_Delete(libraries);
    cJSON_Delete(front.tree);
    finish_base();
}

/** Each layer's plugins consume a capability only the other layer provides, so each layer's session start is refused. */
static void lay_out_two_split_layers(const char *label) {
    start_world(label);
    register_test_host("1.0.0");
    require_front_door("scripted-consumer");
    install_layered_plugin("scripted-consumer", TYPESCRIPT, "{\"provides\":[],\"consumes\":[\"x\"]}", NULL);
    install_layered_plugin("compiled-provider", FIXTURE, "{\"provides\":[\"x\"],\"consumes\":[]}", NULL);
    install_layered_plugin("compiled-consumer", FIXTURE, "{\"provides\":[],\"consumes\":[\"y\"]}", NULL);
    install_layered_plugin("scripted-provider", TYPESCRIPT, "{\"provides\":[\"y\"],\"consumes\":[]}", NULL);
    write_catalog();
    write_settings(application_home,
                   "{\"plugins\":{\"scripted-consumer\":{\"enabled\":true},\"compiled-provider\":{\"enabled\":true},"
                   "\"compiled-consumer\":{\"enabled\":true},\"scripted-provider\":{\"enabled\":true}}}");
}

static void expect_published_refusal(const char *identity, const char *part, const char *label) {
    char rest[256];
    snprintf(rest, sizeof rest, "processes/%s.ready.json", identity);
    char *path = joined(state, rest);
    cJSON *record = read_json_file(path);
    const char *refused = text_at(record, "refused");
    if (refused == NULL || strstr(refused, part) == NULL) {
        fprintf(stderr, "%s: expected %s's record to name %s, got %s\n", label, identity, part,
                refused == NULL ? "(none)" : refused);
        check_failures++;
    }
    cJSON_Delete(record);
    spisor_memory_release(path);
}

/** Each layer's refusal lands on its own record whichever is refused first, never over the other's. */
static void each_layer_publishes_its_refusal_to_its_own_record_in_either_order(void) {
    const char *orders[2][2] = {{TYPESCRIPT, FIXTURE}, {FIXTURE, TYPESCRIPT}};
    for (size_t order = 0; order < 2; order++) {
        lay_out_two_split_layers(order == 0 ? "start-refusals-front-first" : "start-refusals-other-first");
        for (size_t index = 0; index < 2; index++) {
            catalog_answer started = start_layer_in(environment_json(), 1, orders[order][index], NULL);
            CHECK(started.status == SPISOR_REFUSED);
            cJSON_Delete(started.tree);
        }
        expect_published_refusal(TEST_HOST, "scripted-consumer (typescript)", "the front door's own refusal");
        expect_published_refusal(TEST_HOST "." FIXTURE, "compiled-consumer (fixture-one)", "the other layer's own refusal");
        finish_base();
    }
}

int main(void) {
    one_session_applies_once_and_the_other_layer_reuses_its_run();
    each_layer_publishes_its_refusal_to_its_own_record_in_either_order();
    only_the_front_door_layer_checks_for_updates();
    an_action_of_a_plugin_in_the_other_layer_is_refused_naming_its_layer();
    a_run_identity_may_carry_its_layer_after_a_dot();
    each_layer_plans_only_the_plugins_of_its_language();
    a_consumer_whose_only_provider_runs_in_the_other_layer_is_refused();
    an_unknown_layer_language_is_malformed();
    a_start_no_language_can_hold_the_front_door_of_is_refused();
    a_start_whose_first_required_plugin_no_catalog_lists_is_refused();
    an_action_start_plans_the_actions_plugin_without_a_serve_only_plugin_beside_it();
    an_action_start_of_a_consumer_plans_its_provider_too();
    an_action_start_plans_the_providers_of_its_providers();
    the_layer_without_the_front_door_runs_under_its_own_identity();
    a_non_session_start_answers_no_timers();
    a_start_writes_no_hint_for_the_registered_host();
    the_identity_version_is_the_one_the_plan_runs();
    a_second_in_process_start_reuses_the_live_run_beside_a_scheduled_version();
    a_refused_session_start_publishes_its_refusal();
    a_refused_non_session_start_publishes_nothing();
    a_session_start_applies_updates();
    a_non_session_start_reuses_a_live_recorded_run();
    a_non_session_start_plans_afresh_when_the_recorded_run_is_dead();
    a_live_recorded_run_with_a_version_that_is_no_text_is_refused_as_unreadable();
    the_running_map_names_the_host_the_layer_and_every_library();
    an_unregistered_host_is_refused_with_the_reinstall_fix();
    a_registration_without_a_version_is_refused();
    the_settings_come_from_the_registrations_home();
    no_color_counts_only_when_set_and_not_empty();
    the_timers_are_the_runtimes_own();
    no_color_ignores_case_on_windows();
    a_no_color_that_is_no_text_is_refused_as_no_usable_environment();
    a_recorded_plugin_differing_only_in_case_from_a_running_id_is_kept();
    a_live_run_naming_the_host_in_another_case_does_not_keep_its_recorded_run();
    return check_summary("hosts_start_test");
}
