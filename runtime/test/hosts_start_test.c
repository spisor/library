#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "hosts_call.h"
#include "spisor/allocation.h"
#include "updates_fixture.h"

#include "hosts_start.h"
#include "platform_key.h"
#include "run_records.h"

#define TEST_HOST "test-host"
#define LAYER "javascript-host"
#define RECORDED_RUN                                                                                                \
    "{\"test-host\":\"1.0.0\",\"javascript-host\":\"2.0.0\",\"runtime\":\"0.1.0\",\"koffi\":\"2.0.0\",\"a\":\"0.9.0\"}"

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

static void start_world(const char *label) {
    start_base(label);
    snprintf(user_home, sizeof user_home, "%s/user", scratch);
    snprintf(application_home, sizeof application_home, "%s/application", scratch);
    snprintf(portable, sizeof portable, "%s/portable", scratch);
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

static catalog_answer start_host_in(cJSON *environment, int session) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "environment", environment);
    cJSON_AddStringToObject(argument, "hostId", TEST_HOST);
    cJSON_AddItemToObject(argument, "layer", parsed("{\"id\":\"" LAYER "\",\"version\":\"2.0.0\"}"));
    cJSON_AddItemToObject(argument, "libraries", parsed("{\"runtime\":\"0.1.0\",\"koffi\":\"2.0.0\"}"));
    cJSON_AddBoolToObject(argument, "session", session);
    return hosts_call(HOSTS_START, argument);
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
                       "{\"test-host\":\"1.0.0\",\"javascript-host\":\"2.0.0\",\"runtime\":\"0.1.0\",\"koffi\":\"2.0.0\","
                       "\"a\":\"1.0.0\"}",
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
                      "{\"run\":{\"test-host\":\"1.0.0\",\"javascript-host\":\"2.0.0\",\"runtime\":\"0.1.0\",\"koffi\":"
                      "\"2.0.0\",\"a\":\"1.0.0\"},\"next\":{}}",
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

static void no_color_is_set_by_an_empty_no_color(void) {
    start_world("start-no-color");
    register_test_host("1.0.0");
    enable_plugin_a();
    cJSON *environment = environment_json();
    cJSON_AddStringToObject(environment, "NO_COLOR", "");
    catalog_answer colorless = start_host_in(environment, 1);
    expect_success(colorless, "no color");
    CHECK(is_true_at(colorless.tree, "noColor"));
    catalog_answer colored = start_host(1);
    CHECK(is_false_at(colored.tree, "noColor"));
    cJSON_Delete(colored.tree);
    cJSON_Delete(colorless.tree);
    finish_base();
}

/** An environment is the OS's, every value text, so a NO_COLOR that is no text is a malformed call, never colour. */
static void a_no_color_that_is_no_text_is_refused_as_no_usable_environment(void) {
    start_world("start-no-color-number");
    register_test_host("1.0.0");
    enable_plugin_a();
    cJSON *environment = environment_json();
    cJSON_AddNumberToObject(environment, "NO_COLOR", 1);
    cJSON *libraries = parsed("{\"runtime\":\"0.1.0\",\"koffi\":\"2.0.0\"}");
    runtime_hosts_start_request request = {environment, TEST_HOST, LAYER, "2.0.0", libraries, 1, 1};
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
    cJSON_AddStringToObject(run, "Koffi", "3.0.0");
    char *text = cJSON_PrintUnformatted(run);
    char references[1024];
    snprintf(references, sizeof references, "{\"run\":%s,\"next\":{}}", text);
    record_references(TEST_HOST, references);
    spisor_memory_release(written_run_record(state, TEST_HOST, run, RUNTIME_DEFAULT_CHECK_MILLISECONDS));
    catalog_answer started = start_host(0);
    expect_success(started, "plugin case");
    expect_member_json(started.tree, "plan/plugins", "{\"a\":\"0.9.0\",\"Koffi\":\"3.0.0\"}", "case kept");
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
    install_library_here("koffi", "2.0.0");
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
    cJSON *libraries = parsed("{\"runtime\":\"0.1.0\",\"koffi\":\"2.0.0\"}");
    runtime_hosts_start_request request = {environment, TEST_HOST, LAYER, "2.0.0", libraries, 1, 1};
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

int main(void) {
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
    no_color_is_set_by_an_empty_no_color();
    the_timers_are_the_runtimes_own();
    no_color_ignores_case_on_windows();
    a_no_color_that_is_no_text_is_refused_as_no_usable_environment();
    a_recorded_plugin_differing_only_in_case_from_a_running_id_is_kept();
    a_live_run_naming_the_host_in_another_case_does_not_keep_its_recorded_run();
    return check_summary("hosts_start_test");
}
