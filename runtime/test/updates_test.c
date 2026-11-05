#include "updates_fixture.h"

#include "fetch_hooks.h"
#include "file_times.h"
#include "host_references.h"
#include "run_records.h"

#if !defined(_WIN32)
#include <time.h>
#endif

#define LOADER "fixture-loader"
#define FOREBAY_HOST "forebay-host"
#define TEST_HOST "test-host"

static cJSON *no_settings(void) {
    return settings_with("{}");
}

static catalog_answer apply_plain(const char *host, const char *settings_json) {
    return apply_with(host, settings_with(settings_json), "{}");
}

static void remove_run_record(catalog_answer started) {
    const char *record = text_at(started.tree, "runRecord");
    CHECK(record != NULL && spisor_file_exists(record));
    if (record != NULL) {
        CHECK(runtime_remove_file_entry(record) == RUNTIME_ENTRY_DONE);
    }
}

static int folder_exists(const char *rest) {
    return exists_in(cache, rest);
}

static void it_writes_what_a_host_runs_and_what_it_scheduled_for_its_next_start_to_its_own_file(void) {
    start_base("references-write");
    cJSON *run = parsed("{\"a\":\"1.0.0\"}");
    cJSON *next = parsed("{\"a\":\"2.0.0\"}");
    runtime_host_references references = {run, next, NULL, NULL};
    char *problem = NULL;
    CHECK(runtime_record_host_references(state, "claude-code", &references, &problem) == 1);
    free(problem);
    expect_text_in(state, "bundles/claude-code.json",
                   "{\n  \"run\": {\n    \"a\": \"1.0.0\"\n  },\n  \"next\": {\n    \"a\": \"2.0.0\"\n  }\n}\n", "own file");
    cJSON_Delete(run);
    cJSON_Delete(next);
    finish_base();
}

static void it_replaces_what_the_same_host_recorded_before(void) {
    start_base("references-replace");
    record_references("claude-code", "{\"run\":{\"a\":\"1.0.0\"},\"next\":{}}");
    cJSON *run = parsed("{\"a\":\"2.0.0\"}");
    cJSON *next = cJSON_CreateObject();
    cJSON *rollback = cJSON_CreateObject();
    runtime_host_references references = {run, next, rollback, NULL};
    char *problem = NULL;
    CHECK(runtime_record_host_references(state, "claude-code", &references, &problem) == 1);
    free(problem);
    expect_references("claude-code", "{\"run\":{\"a\":\"2.0.0\"},\"next\":{}}", "replaced, an empty rollback left out");
    runtime_host_references read = runtime_read_host_references(state, "claude-code");
    expect_json(read.run, "{\"a\":\"2.0.0\"}", "read back run");
    runtime_free_host_references(&read);
    cJSON_Delete(run);
    cJSON_Delete(next);
    cJSON_Delete(rollback);
    finish_base();
}

static void it_refuses_a_host_id_that_would_write_outside_the_bundles_folder(void) {
    start_base("references-escape");
    cJSON *empty = cJSON_CreateObject();
    runtime_host_references references = {empty, empty, NULL, NULL};
    char *problem = NULL;
    CHECK(runtime_record_host_references(state, "../escape", &references, &problem) == 0);
    CHECK(problem != NULL && strstr(problem, "outside") != NULL);
    free(problem);
    problem = NULL;
    CHECK(runtime_record_host_references(state, "", &references, &problem) == 0);
    CHECK(problem != NULL && strstr(problem, "outside") != NULL);
    free(problem);
    CHECK(!exists_in(state, "escape.json"));
    cJSON_Delete(empty);
    finish_base();
}

static void it_deletes_the_host_s_file_and_every_run_of_the_host_when_its_references_are_removed(void) {
    start_base("references-remove");
    record_references(TEST_HOST, "{\"run\":{\"a\":\"1.0.0\"},\"next\":{}}");
    cJSON *run = parsed("{\"a\":\"1.0.0\"}");
    char *record = runtime_record_run(state, TEST_HOST, run, 5000);
    CHECK(record != NULL && spisor_file_exists(record));
    CHECK(runtime_remove_host_references(state, TEST_HOST) == 1);
    CHECK(!exists_in(state, "bundles/" TEST_HOST ".json"));
    CHECK(!exists_in(state, "bundles/runs/" TEST_HOST));
    CHECK(runtime_remove_host_references(state, "never-recorded") == 1);
    free(record);
    cJSON_Delete(run);
    finish_base();
}

static void it_keeps_the_version_each_of_two_concurrent_starts_runs_and_lets_a_sweep_take_one_once_it_stopped(void) {
    start_base("two-starts");
    write_catalog();
    mark_installed("bundles", "a", "1.0.0");
    catalog_answer first = apply_plain(LOADER, "{\"plugins\":{\"a\":{\"enabled\":true}}}");
    expect_success(first, "first start");
    mark_installed("bundles", "a", "2.0.0");
    catalog_answer second = apply_plain(LOADER, "{\"plugins\":{\"a\":{\"enabled\":true}}}");
    expect_success(second, "second start");
    catalog_answer later = apply_plain(LOADER, "{\"plugins\":{\"a\":{\"enabled\":true}}}");
    remove_run_record(later);
    cJSON_Delete(later.tree);
    expect_references(LOADER, "{\"run\":{\"a\":\"2.0.0\"},\"next\":{}}", "only the first start's run names 1.0.0");
    cJSON_Delete(sweep_cache().tree);
    CHECK(folder_exists("bundles/a/1.0.0") && folder_exists("bundles/a/2.0.0"));

    remove_run_record(first);
    cJSON_Delete(sweep_cache().tree);
    CHECK(!folder_exists("bundles/a/1.0.0") && folder_exists("bundles/a/2.0.0"));
    remove_run_record(second);
    cJSON_Delete(first.tree);
    cJSON_Delete(second.tree);
    finish_base();
}

static void it_records_its_run_as_a_live_record_of_this_process(void) {
    start_base("run-record-shape");
    write_catalog();
    mark_installed("bundles", "a", "1.0.0");
    catalog_answer started = apply_plain(LOADER, "{\"plugins\":{\"a\":{\"enabled\":true}}}");
    const char *record = text_at(started.tree, "runRecord");
    cJSON *recorded = record == NULL ? NULL : read_json_file(record);
    expect_member_json(recorded, "run", "{\"a\":\"1.0.0\"}", "record run");
    CHECK(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(recorded, "pid")) == (double)current_process_id());
    CHECK(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(recorded, "checkMilliseconds")) == 5000);
    CHECK(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(recorded, "aliveAt")));
    cJSON_Delete(recorded);
    remove_run_record(started);
    cJSON_Delete(started.tree);
    finish_base();
}

static void it_resolves_the_enabled_set_and_the_host_s_defaults_each_default_held_to_its_pin(void) {
    start_base("start-defaults");
    write_catalog();
    mark_installed("bundles", "a", "1.0.0");
    mark_installed("bundles", "a", "2.0.0");
    mark_installed("bundles", "d", "1.0.0");
    mark_installed("bundles", "d", "2.0.0");
    catalog_answer started = apply_with(TEST_HOST,
                                        settings_with("{\"plugins\":{\"a\":{\"enabled\":true},\"d\":{\"pin\":\"1.0.0\"}}}"),
                                        "{\"defaults\":{\"demo:greeting\":\"d\"}}");
    expect_success(started, "defaults");
    expect_references(TEST_HOST, "{\"run\":{\"a\":\"2.0.0\",\"d\":\"1.0.0\"},\"next\":{}}", "defaults references");
    expect_member_json(started.tree, "plugins", "{\"a\":\"2.0.0\",\"d\":\"1.0.0\"}", "defaults plugins");
    remove_run_record(started);
    cJSON_Delete(started.tree);
    finish_base();
}

static void it_leaves_out_a_default_the_settings_disable(void) {
    start_base("start-disabled-default");
    write_catalog();
    mark_installed("bundles", "d", "1.0.0");
    catalog_answer started = apply_with(TEST_HOST, settings_with("{\"plugins\":{\"d\":{\"enabled\":false}}}"),
                                        "{\"defaults\":{\"demo:greeting\":\"d\"}}");
    expect_references(TEST_HOST, "{\"run\":{},\"next\":{}}", "disabled default");
    remove_run_record(started);
    cJSON_Delete(started.tree);
    finish_base();
}

static void it_records_the_host_s_own_references_beside_its_plugins(void) {
    start_base("start-own");
    write_catalog();
    mark_installed("bundles", "a", "1.0.0");
    record_references(TEST_HOST, "{\"run\":{},\"next\":{\"runtime\":\"0.0.9\"}}");
    catalog_answer started =
        apply_with(TEST_HOST, settings_with("{\"plugins\":{\"a\":{\"enabled\":true}}}"),
                   "{\"running\":{\"forebay-host\":\"0.1.0\",\"runtime\":\"0.1.0\"}}");
    expect_references(TEST_HOST,
                      "{\"run\":{\"forebay-host\":\"0.1.0\",\"runtime\":\"0.1.0\",\"a\":\"1.0.0\"},\"next\":{\"runtime\":"
                      "\"0.0.9\"}}",
                      "own references");
    remove_run_record(started);
    cJSON_Delete(started.tree);
    finish_base();
}

static void it_switches_to_the_scheduled_host_version_once_it_is_installed_intact_keeping_the_previous(void) {
    start_base("switch");
    write_catalog();
    mark_installed("bundles", LOADER, "1.0.0");
    record_references(LOADER, "{\"run\":{\"" LOADER "\":\"1.0.0\"},\"next\":{\"" LOADER "\":\"2.0.0\",\"a\":\"3.0.0\"}}");
    catalog_answer waiting = apply_plain(LOADER, "{}");
    expect_member_json(waiting.tree, "run", "{\"" LOADER "\":\"1.0.0\"}", "not installed yet");
    expect_references(LOADER,
                      "{\"run\":{\"" LOADER "\":\"1.0.0\"},\"next\":{\"" LOADER "\":\"2.0.0\",\"a\":\"3.0.0\"}}",
                      "still scheduled");
    remove_run_record(waiting);
    cJSON_Delete(waiting.tree);

    mark_installed("bundles", LOADER, "2.0.0");
    catalog_answer switched = apply_plain(LOADER, "{}");
    expect_member_json(switched.tree, "run", "{\"" LOADER "\":\"2.0.0\"}", "switched");
    expect_references(LOADER,
                      "{\"run\":{\"" LOADER "\":\"2.0.0\"},\"next\":{\"a\":\"3.0.0\"},\"rollback\":{\"" LOADER
                      "\":\"1.0.0\"}}",
                      "switched references");
    remove_run_record(switched);
    cJSON_Delete(switched.tree);

    catalog_answer later = apply_plain(LOADER, "{}");
    expect_references(LOADER, "{\"run\":{\"" LOADER "\":\"2.0.0\"},\"next\":{\"a\":\"3.0.0\"}}",
                      "a later start keeps no rollback");
    remove_run_record(later);
    cJSON_Delete(later.tree);
    finish_base();
}

static void install_libraries(void) {
    list_library("runtime", "0.1.0", "0.1.0");
    list_library("runtime", "0.2.0", "0.2.0");
    list_library("koffi", "2.0.0", "2.0.0");
    mark_installed("libraries", "runtime", "0.1.0");
    mark_installed("libraries", "runtime", "0.2.0");
    mark_installed("libraries", "koffi", "2.0.0");
}

static catalog_answer apply_booted(void) {
    return apply_with(FOREBAY_HOST, no_settings(),
                      "{\"running\":{\"forebay-host\":\"1.0.0\",\"runtime\":\"0.1.0\",\"koffi\":\"2.0.0\"},"
                      "\"libraries\":[\"runtime\",\"koffi\"]}");
}

static void it_keeps_what_runs_now_and_schedules_the_libraries_the_next_start_loads_hinting_them(void) {
    start_base("libraries-schedule");
    install_libraries();
    write_catalog();
    catalog_answer started = apply_booted();
    expect_success(started, "libraries");
    expect_references(FOREBAY_HOST,
                      "{\"run\":{\"forebay-host\":\"1.0.0\",\"runtime\":\"0.1.0\",\"koffi\":\"2.0.0\"},\"next\":{"
                      "\"runtime\":\"0.2.0\"}}",
                      "libraries references");
    expect_hint(FOREBAY_HOST, "{\"libraries\":{\"runtime\":\"0.2.0\",\"koffi\":\"2.0.0\"},\"host\":\"1.0.0\"}",
                "libraries hint");
    remove_run_record(started);
    cJSON_Delete(started.tree);
    finish_base();
}

static void it_keeps_the_versions_the_host_already_scheduled_for_its_next_start(void) {
    start_base("libraries-kept");
    install_libraries();
    cJSON_ReplaceItemInObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(catalog_items, "runtime"), "latest",
                                           cJSON_CreateString("0.1.0"));
    write_catalog();
    record_references(FOREBAY_HOST, "{\"run\":{},\"next\":{\"viewer\":\"2.0.0\"}}");
    catalog_answer started = apply_booted();
    expect_member_json(started.tree, "next", "{\"viewer\":\"2.0.0\"}", "kept answer");
    expect_member_json(started.tree, "run", "{\"forebay-host\":\"1.0.0\",\"runtime\":\"0.1.0\",\"koffi\":\"2.0.0\"}",
                       "kept run");
    remove_run_record(started);
    cJSON_Delete(started.tree);
    finish_base();
}

static void it_hints_the_host_version_scheduled_for_the_next_start_only_once_it_is_installed_intact(void) {
    start_base("libraries-host-hint");
    install_libraries();
    write_catalog();
    record_references(FOREBAY_HOST, "{\"run\":{},\"next\":{\"forebay-host\":\"2.0.0\"}}");
    catalog_answer waiting = apply_booted();
    expect_hint(FOREBAY_HOST, "{\"libraries\":{\"runtime\":\"0.2.0\",\"koffi\":\"2.0.0\"},\"host\":\"1.0.0\"}",
                "not installed yet");
    remove_run_record(waiting);
    cJSON_Delete(waiting.tree);

    mark_installed("bundles", FOREBAY_HOST, "2.0.0");
    catalog_answer installed = apply_booted();
    expect_hint(FOREBAY_HOST, "{\"libraries\":{\"runtime\":\"0.2.0\",\"koffi\":\"2.0.0\"},\"host\":\"2.0.0\"}",
                "installed intact");
    remove_run_record(installed);
    cJSON_Delete(installed.tree);
    finish_base();
}

static void it_writes_no_hint_while_a_library_the_host_boots_with_is_not_installed(void) {
    start_base("libraries-missing");
    list_library("runtime", "0.1.0", "0.1.0");
    mark_installed("libraries", "runtime", "0.1.0");
    write_catalog();
    catalog_answer started = apply_booted();
    expect_success(started, "missing library");
    CHECK(!exists_in(cache, "hosts/" FOREBAY_HOST ".json"));
    remove_run_record(started);
    cJSON_Delete(started.tree);
    finish_base();
}

static void list_both_hosts(void) {
    list_host(LOADER, "1.0.0", "2.0.0");
    list_host(LOADER, "2.0.0", "2.0.0");
    list_host(FOREBAY_HOST, "1.0.0", "2.0.0");
    list_host(FOREBAY_HOST, "2.0.0", "2.0.0");
    write_catalog();
    install_listed(LOADER, "1.0.0");
    install_listed(FOREBAY_HOST, "1.0.0");
    write_hint(FOREBAY_HOST, "{\"libraries\":{\"runtime\":\"0.1.0\",\"koffi\":\"1.0.0\"},\"host\":\"1.0.0\"}");
    record_references(LOADER, "{\"run\":{\"" LOADER "\":\"1.0.0\",\"" FOREBAY_HOST "\":\"1.0.0\"},\"next\":{}}");
}

static void it_schedules_a_newer_loader_and_forebay_host_for_the_next_start_which_then_runs_both(void) {
    start_base("host-updates");
    list_both_hosts();
    catalog_answer checked = check_with(LOADER, no_settings(), "{}");
    expect_success(checked, "host updates check");
    expect_member_json(checked.tree, "fetched", "[\"" LOADER "@2.0.0\",\"" FOREBAY_HOST "@2.0.0\"]", "both fetched");
    expect_member_json(checked.tree, "refused", "[]", "none refused");
    cJSON_Delete(checked.tree);
    runtime_host_references recorded = runtime_read_host_references(state, LOADER);
    expect_json(recorded.next, "{\"" LOADER "\":\"2.0.0\",\"" FOREBAY_HOST "\":\"2.0.0\"}", "both scheduled");
    runtime_free_host_references(&recorded);
    expect_hint(FOREBAY_HOST, "{\"libraries\":{\"runtime\":\"0.1.0\",\"koffi\":\"1.0.0\"},\"host\":\"2.0.0\"}",
                "forebay-host hinted, its libraries kept");

    catalog_answer started = apply_plain(LOADER, "{}");
    check_text(text_at(value_at(started.tree, "run"), LOADER), "2.0.0", "the next start runs the new loader");
    remove_run_record(started);
    cJSON_Delete(started.tree);
    catalog_answer resolved = resolve_boot(FOREBAY_HOST, NULL);
    expect_executable(resolved, FOREBAY_HOST, "2.0.0", "every command boots the new forebay-host");
    cJSON_Delete(resolved.tree);
    finish_base();
}

static void it_holds_a_host_to_its_pin(void) {
    start_base("host-pin");
    list_both_hosts();
    catalog_answer checked = check_with(LOADER, settings_with("{\"plugins\":{\"" LOADER "\":{\"pin\":\"1.0.0\"}}}"), "{}");
    expect_member_json(checked.tree, "fetched", "[\"" FOREBAY_HOST "@2.0.0\"]", "pinned fetched");
    cJSON_Delete(checked.tree);
    runtime_host_references recorded = runtime_read_host_references(state, LOADER);
    expect_json(recorded.next, "{\"" FOREBAY_HOST "\":\"2.0.0\"}", "pinned scheduled");
    runtime_free_host_references(&recorded);
    finish_base();
}

static void it_records_a_version_it_could_not_install_as_refused_and_finishes(void) {
    start_base("check-refused");
    list_corrupt_plugin("a", "1.0.0", "1.0.0");
    write_catalog();
    catalog_answer checked = check_with(TEST_HOST, settings_with("{\"plugins\":{\"a\":{\"enabled\":true}}}"), "{}");
    expect_success(checked, "refused check");
    expect_member_json(checked.tree, "fetched", "[]", "nothing fetched");
    const cJSON *refused = cJSON_GetObjectItemCaseSensitive(checked.tree, "refused");
    CHECK(cJSON_GetArraySize(refused) == 1);
    check_text(text_at(cJSON_GetArrayItem(refused, 0), "id"), "a@1.0.0", "refused id");
    CHECK(text_at(cJSON_GetArrayItem(refused, 0), "message") != NULL && text_at(cJSON_GetArrayItem(refused, 0), "fix") != NULL);
    cJSON_Delete(checked.tree);
    finish_base();
}

static void it_records_the_versions_scheduled_for_the_next_start_whether_or_not_they_install(void) {
    start_base("check-records-first");
    list_corrupt_plugin("a", "1.0.0", "1.0.0");
    write_catalog();
    record_references(TEST_HOST, "{\"run\":{\"a\":\"0.9.0\"},\"next\":{}}");
    cJSON_Delete(check_with(TEST_HOST, settings_with("{\"plugins\":{\"a\":{\"enabled\":true}}}"), "{}").tree);
    expect_references(TEST_HOST, "{\"run\":{\"a\":\"0.9.0\"},\"next\":{\"a\":\"1.0.0\"}}", "recorded first");
    finish_base();
}

static void it_refetches_a_damaged_version_the_start_reported_even_one_the_latest_does_not_name(void) {
    start_base("check-repairs");
    list_plugin("a", "1.0.0", "2.0.0");
    list_plugin("a", "2.0.0", "2.0.0");
    write_catalog();
    catalog_answer checked =
        check_with(TEST_HOST, settings_with("{\"plugins\":{\"a\":{\"enabled\":true}}}"),
                   "{\"problems\":[{\"id\":\"a\",\"version\":\"1.0.0\"}]}");
    expect_member_json(checked.tree, "fetched", "[\"a@1.0.0\",\"a@2.0.0\"]", "repaired and fetched");
    cJSON_Delete(checked.tree);
    finish_base();
}

static void strip_marker_sizes(const char *rest) {
    char *text = text_in(cache, rest);
    cJSON *marker = text == NULL ? NULL : spisor_parse_json((const uint8_t *)text, strlen(text));
    CHECK(cJSON_HasObjectItem(marker, "sizes"));
    cJSON_DeleteItemFromObjectCaseSensitive(marker, "sizes");
    char *stripped = cJSON_PrintUnformatted(marker);
    write_in(cache, rest, stripped);
    cJSON_free(stripped);
    cJSON_Delete(marker);
    free(text);
}

/** A running version a start would refuse, here a marker recording no sizes, is installed again by the next check. */
static void it_reinstalls_a_version_whose_marker_records_no_sizes(void) {
    start_base("check-no-sizes");
    list_plugin("a", "1.0.0", "1.0.0");
    write_catalog();
    cJSON_Delete(check_with(TEST_HOST, settings_with("{\"plugins\":{\"a\":{\"enabled\":true}}}"), "{}").tree);
    record_references(TEST_HOST, "{\"run\":{\"a\":\"1.0.0\"},\"next\":{}}");
    strip_marker_sizes("bundles/a/1.0.0/.installed.json");
    cJSON_Delete(check_with(TEST_HOST, settings_with("{\"plugins\":{\"a\":{\"enabled\":true}}}"), "{}").tree);
    char *text = text_in(cache, "bundles/a/1.0.0/.installed.json");
    cJSON *marker = text == NULL ? NULL : spisor_parse_json((const uint8_t *)text, strlen(text));
    CHECK(cJSON_HasObjectItem(marker, "sizes"));
    cJSON_Delete(marker);
    free(text);
    finish_base();
}

static void it_refuses_an_item_no_catalog_source_lists(void) {
    start_base("check-unlisted");
    write_catalog();
    catalog_answer checked = check_with(TEST_HOST, no_settings(), "{\"problems\":[{\"id\":\"gone\",\"version\":\"1.0.0\"}]}");
    const cJSON *refused = cJSON_GetObjectItemCaseSensitive(checked.tree, "refused");
    check_text(text_at(cJSON_GetArrayItem(refused, 0), "id"), "gone@1.0.0", "unlisted id");
    check_text(text_at(cJSON_GetArrayItem(refused, 0), "message"), "no catalog source lists gone", "unlisted message");
    cJSON_Delete(checked.tree);
    finish_base();
}

static void it_sweeps_what_nothing_names_once_it_has_installed(void) {
    start_base("check-sweeps");
    list_plugin("a", "3.0.0", "3.0.0");
    write_catalog();
    mark_installed("bundles", "a", "1.0.0");
    mark_installed("bundles", "a", "2.0.0");
    catalog_answer checked = check_with(TEST_HOST, settings_with("{\"plugins\":{\"a\":{\"enabled\":true}}}"), "{}");
    expect_member_json(checked.tree, "removed", "[\"a@1.0.0\",\"a@2.0.0\"]", "swept");
    CHECK(folder_exists("bundles/a/3.0.0"));
    cJSON_Delete(checked.tree);
    finish_base();
}

static void it_reports_each_catalog_check(void) {
    start_base("check-reports");
    write_catalog();
    catalog_answer checked = check_with(TEST_HOST, no_settings(), "{}");
    expect_member_json(checked.tree, "checked", "[{\"kind\":\"changed\"}]", "checked");
    cJSON_Delete(checked.tree);
    finish_base();
}

/* plan decision 6: the per-host next write takes the registry lock */

typedef struct concurrent_check {
    const char *plugin;
    catalog_answer answer;
} concurrent_check;

static void check_concurrently(concurrent_check *job) {
    char settings[256];
    snprintf(settings, sizeof settings, "{\"plugins\":{\"%s\":{\"enabled\":true}}}", job->plugin);
    job->answer = check_with(TEST_HOST, settings_with(settings), "{}");
}

#if defined(_WIN32)
static DWORD WINAPI checking_entry(LPVOID argument) {
    check_concurrently(argument);
    return 0;
}
#else
static void *checking_entry(void *argument) {
    check_concurrently(argument);
    return NULL;
}
#endif

static void run_two_checks(concurrent_check *jobs) {
#if defined(_WIN32)
    HANDLE threads[2];
    threads[0] = CreateThread(NULL, 0, checking_entry, &jobs[0], 0, NULL);
    threads[1] = CreateThread(NULL, 0, checking_entry, &jobs[1], 0, NULL);
    CHECK(threads[0] != NULL && threads[1] != NULL);
    WaitForMultipleObjects(2, threads, TRUE, INFINITE);
    CloseHandle(threads[0]);
    CloseHandle(threads[1]);
#else
    pthread_t threads[2];
    CHECK(pthread_create(&threads[0], NULL, checking_entry, &jobs[0]) == 0);
    CHECK(pthread_create(&threads[1], NULL, checking_entry, &jobs[1]) == 0);
    pthread_join(threads[0], NULL);
    pthread_join(threads[1], NULL);
#endif
}

static void it_loses_no_next_write_when_two_checks_of_one_host_run_at_once(void) {
    for (int round = 0; round < 12; round++) {
        start_base("concurrent-checks");
        list_plugin("a", "1.0.0", "1.0.0");
        list_plugin("b", "1.0.0", "1.0.0");
        write_catalog();
        concurrent_check jobs[2] = {{"a", {0, NULL}}, {"b", {0, NULL}}};
        run_two_checks(jobs);
        CHECK(jobs[0].answer.status == SPISOR_STATUS_SUCCESS && jobs[1].answer.status == SPISOR_STATUS_SUCCESS);
        cJSON_Delete(jobs[0].answer.tree);
        cJSON_Delete(jobs[1].answer.tree);
        runtime_host_references recorded = runtime_read_host_references(state, TEST_HOST);
        expect_json(recorded.next, "{\"a\":\"1.0.0\",\"b\":\"1.0.0\"}", "both next writes kept");
        runtime_free_host_references(&recorded);
        finish_base();
    }
}

static void it_runs_the_scheduled_version_after_apply_and_a_sweep_keeps_what_the_hint_and_rollback_name(void) {
    start_base("focus-four");
    list_host(FOREBAY_HOST, "1.0.0", "1.0.0");
    write_catalog();
    install_listed(FOREBAY_HOST, "1.0.0");
    write_hint(FOREBAY_HOST, "{\"libraries\":{},\"host\":\"1.0.0\"}");
    record_references(FOREBAY_HOST, "{\"run\":{\"" FOREBAY_HOST "\":\"1.0.0\"},\"next\":{}}");
    catalog_answer before = resolve_boot(FOREBAY_HOST, "[\"status\"]");
    expect_executable(before, FOREBAY_HOST, "1.0.0", "A runs");
    cJSON_Delete(before.tree);

    list_host(FOREBAY_HOST, "2.0.0", "2.0.0");
    write_catalog();
    cJSON_Delete(check_with(FOREBAY_HOST, no_settings(), "{}").tree);
    catalog_answer started = apply_with(FOREBAY_HOST, no_settings(), "{\"libraries\":[]}");
    expect_member_json(started.tree, "run", "{\"" FOREBAY_HOST "\":\"2.0.0\"}", "B runs");
    expect_member_json(started.tree, "rollback", "{\"" FOREBAY_HOST "\":\"1.0.0\"}", "A kept for rollback");
    remove_run_record(started);
    cJSON_Delete(started.tree);
    catalog_answer after = resolve_boot(FOREBAY_HOST, "[\"status\"]");
    expect_executable(after, FOREBAY_HOST, "2.0.0", "B boots");
    cJSON_Delete(after.tree);

    cJSON_Delete(sweep_cache().tree);
    CHECK(folder_exists("bundles/" FOREBAY_HOST "/1.0.0") && folder_exists("bundles/" FOREBAY_HOST "/2.0.0"));
    finish_base();
}

/* updates.rollback */

static void it_rolls_a_switched_host_back_to_the_version_it_switched_from(void) {
    start_base("rollback");
    list_host(FOREBAY_HOST, "1.0.0", "2.0.0");
    list_host(FOREBAY_HOST, "2.0.0", "2.0.0");
    write_catalog();
    install_listed(FOREBAY_HOST, "1.0.0");
    install_listed(FOREBAY_HOST, "2.0.0");
    write_hint(FOREBAY_HOST, "{\"libraries\":{\"runtime\":\"0.1.0\"},\"host\":\"2.0.0\"}");
    record_references(FOREBAY_HOST, "{\"run\":{\"" FOREBAY_HOST "\":\"2.0.0\"},\"next\":{},\"rollback\":{\"" FOREBAY_HOST
                                    "\":\"1.0.0\"}}");
    catalog_answer rolled = rollback_of(FOREBAY_HOST, FOREBAY_HOST);
    expect_success(rolled, "rollback");
    expect_member_json(rolled.tree, "run", "{\"" FOREBAY_HOST "\":\"1.0.0\"}", "rolled run");
    expect_references(FOREBAY_HOST,
                      "{\"run\":{\"" FOREBAY_HOST "\":\"1.0.0\"},\"next\":{},\"rollback\":{\"" FOREBAY_HOST
                      "\":\"2.0.0\"},\"rolledBackFrom\":{\"" FOREBAY_HOST "\":\"2.0.0\"}}",
                      "rolled references");
    expect_hint(FOREBAY_HOST, "{\"libraries\":{\"runtime\":\"0.1.0\"},\"host\":\"1.0.0\"}", "rolled hint");
    cJSON_Delete(rolled.tree);
    catalog_answer resolved = resolve_boot(FOREBAY_HOST, NULL);
    expect_executable(resolved, FOREBAY_HOST, "1.0.0", "rolled boots");
    cJSON_Delete(resolved.tree);
    finish_base();
}

static void it_refuses_a_rollback_no_switch_kept_a_version_for(void) {
    start_base("rollback-none");
    record_references(FOREBAY_HOST, "{\"run\":{\"" FOREBAY_HOST "\":\"2.0.0\"},\"next\":{}}");
    catalog_answer rolled = rollback_of(FOREBAY_HOST, FOREBAY_HOST);
    expect_refusal(rolled, "no version of forebay-host is kept for rollback", "no rollback");
    cJSON_Delete(rolled.tree);
    finish_base();
}

static void it_refuses_a_rollback_to_a_version_that_is_no_longer_installed_intact(void) {
    start_base("rollback-gone");
    record_references(FOREBAY_HOST, "{\"run\":{\"" FOREBAY_HOST "\":\"2.0.0\"},\"next\":{},\"rollback\":{\"" FOREBAY_HOST
                                    "\":\"1.0.0\"}}");
    catalog_answer rolled = rollback_of(FOREBAY_HOST, FOREBAY_HOST);
    expect_refusal(rolled, "is not installed intact", "rollback gone");
    expect_references(FOREBAY_HOST,
                      "{\"run\":{\"" FOREBAY_HOST "\":\"2.0.0\"},\"next\":{},\"rollback\":{\"" FOREBAY_HOST
                      "\":\"1.0.0\"}}",
                      "nothing changed");
    cJSON_Delete(rolled.tree);
    finish_base();
}

static void it_refuses_a_host_id_that_is_no_plain_file_name(void) {
    start_base("escape-id");
    expect_refusal(apply_plain("../escape", "{}"), "outside", "apply escape");
    expect_refusal(check_with("../escape", no_settings(), "{}"), "outside", "check escape");
    expect_refusal(rollback_of("../escape", "a"), "outside", "rollback escape");
    finish_base();
}

static void pause_milliseconds(unsigned milliseconds) {
#if defined(_WIN32)
    Sleep(milliseconds);
#else
    struct timespec pause = {(time_t)(milliseconds / 1000), (long)(milliseconds % 1000) * 1000000L};
    nanosleep(&pause, NULL);
#endif
}

static int references_name(const char *host, const char *fragment) {
    char rest[512];
    snprintf(rest, sizeof rest, "bundles/%s.json", host);
    char *text = text_in(state, rest);
    int named = text != NULL && strstr(text, fragment) != NULL;
    free(text);
    return named;
}

#define STALL_MILLISECONDS 3000
#define START_BOUND_MILLISECONDS 1500.0
#define UNWRITABLE_BOUND_MILLISECONDS 500.0

static void it_lets_a_start_finish_while_a_check_s_download_is_stalled(void) {
    start_base("stalled-download");
    list_plugin("slow", "1.0.0", "1.0.0");
    write_catalog();
    runtime_fetch_stall_file_urls("/slow/", STALL_MILLISECONDS);
    concurrent_check job = {"slow", {0, NULL}};
#if defined(_WIN32)
    HANDLE thread = CreateThread(NULL, 0, checking_entry, &job, 0, NULL);
    CHECK(thread != NULL);
#else
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, checking_entry, &job) == 0);
#endif
    for (int waited = 0; waited < 100 && !references_name(TEST_HOST, "slow"); waited++) {
        pause_milliseconds(50);
    }
    CHECK(references_name(TEST_HOST, "slow"));
    double started_at = runtime_now_milliseconds();
    catalog_answer started = apply_plain(TEST_HOST, "{}");
    double took = runtime_now_milliseconds() - started_at;
    expect_success(started, "start beside a stalled download");
    if (took >= START_BOUND_MILLISECONDS) {
        fprintf(stderr, "the start waited %.0f ms behind a stalled download\n", took);
        check_failures++;
    }
    remove_run_record(started);
    cJSON_Delete(started.tree);
#if defined(_WIN32)
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
#else
    pthread_join(thread, NULL);
#endif
    runtime_fetch_stall_file_urls(NULL, 0);
    expect_member_json(job.answer.tree, "fetched", "[\"slow@1.0.0\"]", "the stalled download completes");
    cJSON_Delete(job.answer.tree);
    finish_base();
}

static void install_both_forebay_hosts(void) {
    list_host(FOREBAY_HOST, "1.0.0", "2.0.0");
    list_host(FOREBAY_HOST, "2.0.0", "2.0.0");
    write_catalog();
    install_listed(FOREBAY_HOST, "1.0.0");
    install_listed(FOREBAY_HOST, "2.0.0");
    write_hint(FOREBAY_HOST, "{\"libraries\":{},\"host\":\"2.0.0\"}");
    record_references(FOREBAY_HOST, "{\"run\":{\"" FOREBAY_HOST "\":\"2.0.0\"},\"next\":{},\"rollback\":{\"" FOREBAY_HOST
                                    "\":\"1.0.0\"}}");
}

static void it_holds_a_rollback_until_the_catalog_moves_past_the_version_rolled_away_from(void) {
    start_base("rollback-holds");
    install_both_forebay_hosts();
    cJSON_Delete(rollback_of(FOREBAY_HOST, FOREBAY_HOST).tree);
    catalog_answer held = check_with(FOREBAY_HOST, no_settings(), "{}");
    expect_member_json(held.tree, "fetched", "[]", "nothing scheduled back");
    cJSON_Delete(held.tree);
    expect_references(FOREBAY_HOST,
                      "{\"run\":{\"" FOREBAY_HOST "\":\"1.0.0\"},\"next\":{},\"rollback\":{\"" FOREBAY_HOST
                      "\":\"2.0.0\"},\"rolledBackFrom\":{\"" FOREBAY_HOST "\":\"2.0.0\"}}",
                      "the rollback holds");
    expect_hint(FOREBAY_HOST, "{\"libraries\":{},\"host\":\"1.0.0\"}", "the hint holds");

    list_host(FOREBAY_HOST, "3.0.0", "3.0.0");
    write_catalog();
    catalog_answer moved = check_with(FOREBAY_HOST, no_settings(), "{}");
    expect_member_json(moved.tree, "fetched", "[\"" FOREBAY_HOST "@3.0.0\"]", "a newer latest is scheduled");
    cJSON_Delete(moved.tree);
    runtime_host_references recorded = runtime_read_host_references(state, FOREBAY_HOST);
    expect_json(recorded.next, "{\"" FOREBAY_HOST "\":\"3.0.0\"}", "scheduled");
    CHECK(recorded.rolled_back_from == NULL);
    runtime_free_host_references(&recorded);
    expect_hint(FOREBAY_HOST, "{\"libraries\":{},\"host\":\"3.0.0\"}", "hinted once the catalog moved");
    finish_base();
}

static void it_rolls_a_plugin_back_and_holds_it_on_the_next_start(void) {
    start_base("plugin-rollback");
    list_plugin("a", "1.0.0", "2.0.0");
    list_plugin("a", "2.0.0", "2.0.0");
    write_catalog();
    mark_installed("bundles", "a", "1.0.0");
    const char *enabled = "{\"plugins\":{\"a\":{\"enabled\":true}}}";
    catalog_answer first = apply_plain(LOADER, enabled);
    expect_member_json(first.tree, "plugins", "{\"a\":\"1.0.0\"}", "runs the installed one");
    remove_run_record(first);
    cJSON_Delete(first.tree);
    mark_installed("bundles", "a", "2.0.0");
    catalog_answer second = apply_plain(LOADER, enabled);
    expect_member_json(second.tree, "plugins", "{\"a\":\"2.0.0\"}", "switches to the latest");
    expect_member_json(second.tree, "rollback", "{\"a\":\"1.0.0\"}", "keeps the previous as rollback");
    remove_run_record(second);
    cJSON_Delete(second.tree);
    catalog_answer rolled = rollback_of(LOADER, "a");
    expect_success(rolled, "plugin rollback");
    expect_member_json(rolled.tree, "run", "{\"a\":\"1.0.0\"}", "rolled run");
    cJSON_Delete(rolled.tree);
    catalog_answer third = apply_plain(LOADER, enabled);
    expect_member_json(third.tree, "plugins", "{\"a\":\"1.0.0\"}", "the rollback holds on the next start");
    remove_run_record(third);
    cJSON_Delete(third.tree);
    finish_base();
}

static void it_reports_a_hint_it_could_not_write_and_still_starts(void) {
    start_base("hint-unwritable");
    install_libraries();
    write_catalog();
    char *blocker = joined(cache, "hosts/" FOREBAY_HOST ".json");
    CHECK(spisor_make_directories(blocker) == 1);
    free(blocker);
    double started_at = runtime_now_milliseconds();
    catalog_answer started = apply_booted();
    double took = runtime_now_milliseconds() - started_at;
    expect_success(started, "start with an unwritable hint");
    if (took >= UNWRITABLE_BOUND_MILLISECONDS) {
        fprintf(stderr, "a hint blocked by a directory held the start for %.0f ms\n", took);
        check_failures++;
    }
    const cJSON *unrecorded = cJSON_GetObjectItemCaseSensitive(started.tree, "unrecorded");
    CHECK(cJSON_GetArraySize(unrecorded) == 1);
    const char *message = text_at(cJSON_GetArrayItem(unrecorded, 0), "message");
    CHECK(message != NULL && strstr(message, "could not be written") != NULL);
    remove_run_record(started);
    cJSON_Delete(started.tree);
    finish_base();
}

static void it_passes_on_the_refusal_of_a_service_it_calls(void) {
    start_base("inner-refusal");
    write_catalog();
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "places", places_json());
    cJSON_AddStringToObject(argument, "hostId", TEST_HOST);
    cJSON_AddStringToObject(argument, "now", "yesterday");
    cJSON_AddItemToObject(argument, "settings", no_settings());
    catalog_answer checked = call_catalog(runtime_updates_check, argument);
    expect_refusal(checked, "no usable now", "catalog.check's own refusal");
    cJSON_Delete(checked.tree);
    finish_base();
}

static void it_schedules_no_fetch_of_a_plugin_version_a_rollback_moved_away_from(void) {
    start_base("apply-filters-rolled-away");
    list_plugin("a", "1.0.0", "2.0.0");
    list_plugin("a", "2.0.0", "2.0.0");
    write_catalog();
    mark_installed("bundles", "a", "1.0.0");
    record_references(LOADER, "{\"run\":{\"a\":\"1.0.0\"},\"next\":{},\"rolledBackFrom\":{\"a\":\"2.0.0\"}}");
    catalog_answer started = apply_plain(LOADER, "{\"plugins\":{\"a\":{\"enabled\":true}}}");
    expect_member_json(started.tree, "next", "{}", "the rolled-away version is not scheduled again");
    remove_run_record(started);
    cJSON_Delete(started.tree);
    finish_base();
}

static void it_drops_a_next_naming_the_version_a_rollback_moves_away_from(void) {
    start_base("rollback-drops-next");
    install_both_forebay_hosts();
    record_references(FOREBAY_HOST, "{\"run\":{\"" FOREBAY_HOST "\":\"2.0.0\"},\"next\":{\"" FOREBAY_HOST
                                    "\":\"2.0.0\"},\"rollback\":{\"" FOREBAY_HOST "\":\"1.0.0\"}}");
    catalog_answer rolled = rollback_of(FOREBAY_HOST, FOREBAY_HOST);
    expect_member_json(rolled.tree, "next", "{}", "next no longer names the version rolled away from");
    cJSON_Delete(rolled.tree);
    catalog_answer started = apply_with(FOREBAY_HOST, no_settings(), "{}");
    expect_member_json(started.tree, "run", "{\"" FOREBAY_HOST "\":\"1.0.0\"}", "the next start stays rolled back");
    remove_run_record(started);
    cJSON_Delete(started.tree);
    finish_base();
}

static void it_never_schedules_under_the_lock_a_version_a_rollback_moved_away_from(void) {
    start_base("locked-schedule-filters");
    install_both_forebay_hosts();
    cJSON_Delete(rollback_of(FOREBAY_HOST, FOREBAY_HOST).tree);
    catalog_answer checked = check_with(FOREBAY_HOST, no_settings(),
                                        "{\"problems\":[{\"id\":\"" FOREBAY_HOST "\",\"version\":\"2.0.0\"}]}");
    expect_success(checked, "repair check");
    cJSON_Delete(checked.tree);
    runtime_host_references recorded = runtime_read_host_references(state, FOREBAY_HOST);
    expect_json(recorded.next, "{}", "a repair of the rolled-away version is not scheduled to run");
    runtime_free_host_references(&recorded);
    finish_base();
}

int main(void) {
    it_writes_what_a_host_runs_and_what_it_scheduled_for_its_next_start_to_its_own_file();
    it_replaces_what_the_same_host_recorded_before();
    it_refuses_a_host_id_that_would_write_outside_the_bundles_folder();
    it_deletes_the_host_s_file_and_every_run_of_the_host_when_its_references_are_removed();

    it_keeps_the_version_each_of_two_concurrent_starts_runs_and_lets_a_sweep_take_one_once_it_stopped();
    it_records_its_run_as_a_live_record_of_this_process();

    it_resolves_the_enabled_set_and_the_host_s_defaults_each_default_held_to_its_pin();
    it_leaves_out_a_default_the_settings_disable();
    it_records_the_host_s_own_references_beside_its_plugins();
    it_switches_to_the_scheduled_host_version_once_it_is_installed_intact_keeping_the_previous();

    it_keeps_what_runs_now_and_schedules_the_libraries_the_next_start_loads_hinting_them();
    it_keeps_the_versions_the_host_already_scheduled_for_its_next_start();
    it_hints_the_host_version_scheduled_for_the_next_start_only_once_it_is_installed_intact();
    it_writes_no_hint_while_a_library_the_host_boots_with_is_not_installed();

    it_schedules_a_newer_loader_and_forebay_host_for_the_next_start_which_then_runs_both();
    it_holds_a_host_to_its_pin();

    it_records_a_version_it_could_not_install_as_refused_and_finishes();
    it_records_the_versions_scheduled_for_the_next_start_whether_or_not_they_install();
    it_refetches_a_damaged_version_the_start_reported_even_one_the_latest_does_not_name();
    it_reinstalls_a_version_whose_marker_records_no_sizes();
    it_refuses_an_item_no_catalog_source_lists();
    it_sweeps_what_nothing_names_once_it_has_installed();
    it_reports_each_catalog_check();
    it_loses_no_next_write_when_two_checks_of_one_host_run_at_once();

    it_runs_the_scheduled_version_after_apply_and_a_sweep_keeps_what_the_hint_and_rollback_name();
    it_rolls_a_switched_host_back_to_the_version_it_switched_from();
    it_refuses_a_rollback_no_switch_kept_a_version_for();
    it_refuses_a_rollback_to_a_version_that_is_no_longer_installed_intact();
    it_refuses_a_host_id_that_is_no_plain_file_name();

    it_lets_a_start_finish_while_a_check_s_download_is_stalled();
    it_holds_a_rollback_until_the_catalog_moves_past_the_version_rolled_away_from();
    it_rolls_a_plugin_back_and_holds_it_on_the_next_start();
    it_reports_a_hint_it_could_not_write_and_still_starts();
    it_passes_on_the_refusal_of_a_service_it_calls();

    it_schedules_no_fetch_of_a_plugin_version_a_rollback_moved_away_from();
    it_drops_a_next_naming_the_version_a_rollback_moves_away_from();
    it_never_schedules_under_the_lock_a_version_a_rollback_moved_away_from();
    return check_summary("updates_test");
}
