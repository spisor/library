#include "host_fixture.h"

static void runs_the_application_with_its_arguments_answers_its_exit_code_and_the_background_host_stops(void) {
    host_world world;
    install_launch_world(&world, "launch", "exit-code", 0);
    program launcher;
    const char *passed[] = {"only", "7", "--keep", NULL};
    launch(&world, &launcher, passed, NULL);
    cJSON *started = await_application(&world, "only");
    long background = await_holders(&world, BACKGROUND_ID, 2);
    expect_json(cJSON_GetObjectItemCaseSensitive(started, "argv"), "[\"only\",\"7\",\"--keep\"]", "arguments");
    expect_holders(&world, BACKGROUND_ID, launcher.child.pid, started_pid(started), "held by launcher and application");
    CHECK(runtime_process_is_alive((double)background));
    cJSON *references = references_of(&world, LOADER_ID);
    check_text(text_at(cJSON_GetObjectItemCaseSensitive(references, "run"), LOADER_ID), "1.0.0", "run recorded");
    cJSON *session = holders_of(&world, LOADER_ID);
    CHECK(session == NULL);
    release_application(&world, "only");
    CHECK(program_finish(&launcher, 30000) == 7);
    CHECK(gone_within(background, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    CHECK(!exists_in(world.base.state, "processes/" BACKGROUND_ID ".json"));
    stop_if_alive(background);
    stop_if_alive(started_pid(started));
    cJSON_Delete(session);
    cJSON_Delete(references);
    cJSON_Delete(started);
    program_free(&launcher);
    finish_world(&world);
}

static void hands_the_application_every_argument_after_the_loader_id_a_leading_dash_dash_included(void) {
    host_world world;
    install_launch_world(&world, "launch", "dash-dash", 0);
    program launcher;
    const char *passed[] = {"--", "0", "x", NULL};
    launch(&world, &launcher, passed, NULL);
    cJSON *started = await_application(&world, "--");
    expect_json(cJSON_GetObjectItemCaseSensitive(started, "argv"), "[\"--\",\"0\",\"x\"]", "dash dash");
    long background = await_holders(&world, BACKGROUND_ID, 2);
    release_application(&world, "--");
    CHECK(program_finish(&launcher, 30000) == 0);
    CHECK(gone_within(background, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    stop_if_alive(background);
    cJSON_Delete(started);
    program_free(&launcher);
    finish_world(&world);
}

static void shares_one_background_process_between_two_launchers(void) {
    host_world world;
    install_launch_world(&world, "launch", "shared", 0);
    program first;
    program second;
    const char *first_arguments[] = {"first", "0", NULL};
    const char *second_arguments[] = {"second", "0", NULL};
    launch(&world, &first, first_arguments, NULL);
    cJSON *first_started = await_application(&world, "first");
    long background = await_holders(&world, BACKGROUND_ID, 2);
    launch(&world, &second, second_arguments, NULL);
    cJSON *second_started = await_application(&world, "second");
    CHECK(await_holders(&world, BACKGROUND_ID, 4) == background);
    release_application(&world, "first");
    CHECK(program_finish(&first, 30000) == 0);
    pause_milliseconds(INTERVAL_MILLISECONDS * 3);
    CHECK(runtime_process_is_alive((double)background));
    expect_holders(&world, BACKGROUND_ID, second.child.pid, started_pid(second_started), "kept for the second");
    release_application(&world, "second");
    CHECK(program_finish(&second, 30000) == 0);
    CHECK(gone_within(background, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    stop_if_alive(background);
    cJSON_Delete(second_started);
    cJSON_Delete(first_started);
    program_free(&second);
    program_free(&first);
    finish_world(&world);
}

static void keeps_the_application_and_its_background_host_after_a_hard_killed_launcher(void) {
    host_world world;
    install_launch_world(&world, "launch", "hard-kill", 0);
    program launcher;
    const char *passed[] = {"orphaned", "0", NULL};
    launch(&world, &launcher, passed, NULL);
    cJSON *started = await_application(&world, "orphaned");
    long application = started_pid(started);
    long background = await_holders(&world, BACKGROUND_ID, 2);
    kill_child(&launcher.child);
    release_child(&launcher.child);
    pause_milliseconds(INTERVAL_MILLISECONDS * 3);
    CHECK(runtime_process_is_alive((double)application));
    CHECK(runtime_process_is_alive((double)background));
    expect_holders(&world, BACKGROUND_ID, application, 0, "held by the application alone");
    double released_at = runtime_now_milliseconds();
    release_application(&world, "orphaned");
    CHECK(gone_within(background, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    CHECK(runtime_now_milliseconds() - released_at < INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE);
    stop_if_alive(background);
    stop_if_alive(application);
    cJSON_Delete(started);
    finish_world(&world);
}

static void refuses_an_application_that_is_not_on_the_path_leaving_nothing_held(void) {
    host_world world;
    install_launch_world(&world, "launch", "absent", 0);
    char *empty = joined(world.base.root, "empty-path");
    CHECK(spisor_make_directories(empty) == 1);
    program launcher;
    const char *passed[] = {"absent", "0", NULL};
    launch(&world, &launcher, passed, empty);
    CHECK(program_finish(&launcher, 30000) == 1);
    expect_contains(launcher.err, APPLICATION_ID " is not on the PATH", "not on the PATH");
    expect_contains(launcher.err, "fix: ", "with a fix");
    CHECK(!exists_in(world.base.state, "processes/" BACKGROUND_ID ".json"));
    program_free(&launcher);
    free(empty);
    finish_world(&world);
}

static void run_to_end(host_world *world, const char *tag) {
    program launcher;
    const char *passed[] = {tag, "0", NULL};
    launch(world, &launcher, passed, NULL);
    cJSON *started = await_application(world, tag);
    long background = await_holders(world, BACKGROUND_ID, 2);
    release_application(world, tag);
    CHECK(program_finish(&launcher, 30000) == 0);
    CHECK(gone_within(background, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    stop_if_alive(background);
    cJSON_Delete(started);
    program_free(&launcher);
}

static void runs_the_loader_version_its_references_schedule_keeping_the_one_it_ran_for_rollback(void) {
    host_world world;
    install_launch_world(&world, "launch", "switch", 0);
    schedule_second_loader(&world);
    run_to_end(&world, "switching");
    cJSON *switched = references_of(&world, LOADER_ID);
    check_text(text_at(cJSON_GetObjectItemCaseSensitive(switched, "run"), LOADER_ID), "2.0.0", "switched run");
    expect_json(cJSON_GetObjectItemCaseSensitive(switched, "rollback"), "{\"" LOADER_ID "\":\"1.0.0\"}", "rollback kept");
    CHECK(text_at(cJSON_GetObjectItemCaseSensitive(switched, "next"), LOADER_ID) == NULL);
    cJSON *registration = registration_in(&world, LOADER_ID);
    check_text(text_at(registration, "version"), "2.0.0", "registered version");
    sweep(&world);
    CHECK(exists_in(world.base.cache, "bundles/" LOADER_ID "/1.0.0"));
    run_to_end(&world, "later");
    cJSON *later = references_of(&world, LOADER_ID);
    CHECK(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(later, "rollback"), LOADER_ID) == NULL);
    sweep(&world);
    CHECK(!exists_in(world.base.cache, "bundles/" LOADER_ID "/1.0.0"));
    CHECK(exists_in(world.base.cache, "bundles/" LOADER_ID "/2.0.0"));
    cJSON_Delete(later);
    cJSON_Delete(registration);
    cJSON_Delete(switched);
    finish_world(&world);
}

#if !defined(_WIN32)
static void passes_a_sigterm_on_to_the_application_and_answers_128_plus_its_number(void) {
    host_world world;
    install_launch_world(&world, "launch", "sigterm", 0);
    program launcher;
    const char *passed[] = {"terminated", "0", NULL};
    launch(&world, &launcher, passed, NULL);
    cJSON *started = await_application(&world, "terminated");
    long background = await_holders(&world, BACKGROUND_ID, 2);
    kill((pid_t)launcher.child.pid, SIGTERM);
    CHECK(program_finish(&launcher, 30000) == 143);
    CHECK(gone_within(started_pid(started), 5000));
    CHECK(gone_within(background, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    stop_if_alive(background);
    cJSON_Delete(started);
    program_free(&launcher);
    finish_world(&world);
}
#endif

static void starts_the_loader_s_session_host_on_javascript_host_when_its_settings_enable_plugins(void) {
    host_world world;
    install_launch_world(&world, "launch", "session", 1);
    world.layer_delay = "1000";
    program launcher;
    const char *passed[] = {"session", "0", NULL};
    launch(&world, &launcher, passed, NULL);
    cJSON *started = await_application(&world, "session");
    long session = await_holders(&world, LOADER_ID, 2);
    long background = await_holders(&world, BACKGROUND_ID, 2);
    expect_holders(&world, LOADER_ID, launcher.child.pid, started_pid(started), "session held");
    char *starts = await_starts_holding(&world, "host.js " LOADER_ID);
    expect_contains(starts, "javascript-host", "session host on javascript-host");
    expect_contains(starts, "host.js " LOADER_ID, "its entry run for the loader");
    release_application(&world, "session");
    CHECK(program_finish(&launcher, 30000) == 0);
    CHECK(gone_within(session, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    CHECK(gone_within(background, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    stop_if_alive(session);
    stop_if_alive(background);
    free(starts);
    cJSON_Delete(started);
    program_free(&launcher);
    finish_world(&world);
}

/**
 * Makes the shim of the loader's one required command unrewritable, as a running image was before it could
 * be set aside: its record asks for new content, and a directory stands where the shim was.
 */
static void make_shim_unrewritable(const host_world *world) {
    char *path = joined(world->base.state, "hosts/" LOADER_ID ".json");
    cJSON *registration = json_in(world->base.state, "hosts/" LOADER_ID ".json");
    cJSON *change;
    cJSON_ArrayForEach(change, cJSON_GetObjectItemCaseSensitive(registration, "changes")) {
        const char *changed = text_at(change, "path");
        if (changed != NULL && strstr(changed, "fx") != NULL && cJSON_HasObjectItem(change, "digest")) {
            cJSON_ReplaceItemInObjectCaseSensitive(change, "digest", cJSON_CreateString("stale"));
            CHECK(runtime_remove_file_entry(changed) == RUNTIME_ENTRY_DONE);
            CHECK(spisor_make_directories(changed) == 1);
        }
    }
    char *text = cJSON_PrintUnformatted(registration);
    write_file(path, text);
    cJSON_free(text);
    cJSON_Delete(registration);
    free(path);
}

static void runs_the_application_when_the_loader_cannot_be_registered_again_saying_why(void) {
    host_world world;
    start_world_at(&world, "launch", "unregistrable");
    install_fake_application(&world);
    install_base *base = &world.base;
    list_native_forebay_host(base, "1.0.0");
    list_library(base, "runtime", "0.1.0");
    list_loader(base, LOADER_ID, "1.0.0", application_json(APPLICATION_ID, APPLICATION_ID, "{}"), "[\"fx\"]", 1,
                "[\"" BACKGROUND_ID "\"]");
    list_native_background(base, BACKGROUND_ID, 1);
    write_catalog(base);
    CHECK(world_install(&world, NULL, NULL) == 0);
    make_shim_unrewritable(&world);
    program launcher;
    const char *passed[] = {"unregistrable", "6", NULL};
    launch(&world, &launcher, passed, NULL);
    cJSON *started = await_application(&world, "unregistrable");
    long background = await_holders(&world, BACKGROUND_ID, 2);
    release_application(&world, "unregistrable");
    CHECK(program_finish(&launcher, 30000) == 6);
    expect_contains(launcher.err, "spisor could not register " LOADER_ID " again: ", "the problem named");
    expect_contains(launcher.err, "fix: ", "with its fix");
    CHECK(gone_within(background, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    stop_if_alive(background);
    cJSON_Delete(started);
    program_free(&launcher);
    finish_world(&world);
}

static void starts_the_session_host_on_a_javascript_runtime_found_again_when_the_recorded_one_is_gone(void) {
    host_world world;
    install_launch_world(&world, "launch", "node-gone", 1);
    char *recorded = joined(world.base.root, "node-bin/node" EXECUTABLE_ENDING);
    char *again = joined(world.base.root, "node-again");
    char *moved = joined(again, "node" EXECUTABLE_ENDING);
    CHECK(spisor_make_directories(again) == 1);
    copy_file(LAYER_FIXTURE_PATH, moved);
    CHECK(runtime_remove_file_entry(recorded) == RUNTIME_ENTRY_DONE);
    char path[4096];
    snprintf(path, sizeof path, "%s" PATH_SEPARATOR "%s", world.bin, again);
    program launcher;
    const char *passed[] = {"again", "0", NULL};
    launch(&world, &launcher, passed, path);
    cJSON *started = await_application(&world, "again");
    long session = await_holders(&world, LOADER_ID, 2);
    long background = await_holders(&world, BACKGROUND_ID, 2);
    char *starts = await_starts_holding(&world, "host.js " LOADER_ID);
    expect_contains(starts, "host.js " LOADER_ID, "session host started");
    cJSON *record = json_in(world.base.cache, "runtimes/javascript.json");
    expect_contains(text_at(record, "executable"), "node-again", "the runtime found again is recorded");
    release_application(&world, "again");
    CHECK(program_finish(&launcher, 30000) == 0);
    CHECK(gone_within(session, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    CHECK(gone_within(background, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    stop_if_alive(session);
    stop_if_alive(background);
    cJSON_Delete(record);
    free(starts);
    cJSON_Delete(started);
    free(moved);
    free(again);
    free(recorded);
    program_free(&launcher);
    finish_world(&world);
}

static void starts_the_session_host_on_its_bundle_s_sizes_without_hashing_it(void) {
    host_world world;
    install_launch_world(&world, "launch", "session-sizes", 1);
    char *script = joined(world.base.cache, "bundles/javascript-host/1.0.0/host.js");
    write_file(script, "export {}:\n");
    program launcher;
    const char *passed[] = {"sized", "0", NULL};
    launch(&world, &launcher, passed, NULL);
    cJSON *started = await_application(&world, "sized");
    long session = await_holders(&world, LOADER_ID, 2);
    long background = await_holders(&world, BACKGROUND_ID, 2);
    char *starts = await_starts_holding(&world, "host.js " LOADER_ID);
    expect_contains(starts, "host.js " LOADER_ID, "session host started on sizes");
    release_application(&world, "sized");
    CHECK(program_finish(&launcher, 30000) == 0);
    CHECK(gone_within(session, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    CHECK(gone_within(background, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    stop_if_alive(session);
    stop_if_alive(background);
    free(starts);
    cJSON_Delete(started);
    free(script);
    program_free(&launcher);
    finish_world(&world);
}

/** A file where acquired runtimes are unpacked, so acquiring the pinned one is refused before any download. */
static void block_runtime_acquisition(const host_world *world) {
    char *folder = joined(world->base.cache, "runtimes/node");
    CHECK(runtime_remove_tree(folder) == 1);
    write_file(folder, "no runtime may be unpacked here");
    free(folder);
}

static void runs_the_application_without_its_session_host_when_no_javascript_runtime_can_be_found(void) {
    host_world world;
    install_launch_world(&world, "launch", "no-javascript", 1);
    char *record = joined(world.base.cache, "runtimes/javascript.json");
    char *node = joined(world.base.root, "node-bin/node" EXECUTABLE_ENDING);
    CHECK(runtime_remove_file_entry(record) == RUNTIME_ENTRY_DONE);
    CHECK(runtime_remove_file_entry(node) == RUNTIME_ENTRY_DONE);
    block_runtime_acquisition(&world);
    program launcher;
    const char *passed[] = {"bare", "0", NULL};
    launch(&world, &launcher, passed, NULL);
    cJSON *started = await_application(&world, "bare");
    long background = await_holders(&world, BACKGROUND_ID, 2);
    release_application(&world, "bare");
    CHECK(program_finish(&launcher, 30000) == 0);
    expect_contains(launcher.err, "could not start the session host of " LOADER_ID, "reported");
    expect_contains(launcher.err, "fix: ", "with its fix");
    CHECK(!exists_in(world.base.state, "processes/" LOADER_ID ".json"));
    CHECK(gone_within(background, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    stop_if_alive(background);
    cJSON_Delete(started);
    free(node);
    free(record);
    program_free(&launcher);
    finish_world(&world);
}

static void write_loader_setting(const host_world *world, const char *content) {
    cJSON *registration = registration_in(world, LOADER_ID);
    char *file = joined(text_at(registration, "home"), "config/settings.json");
    write_file(file, content);
    free(file);
    cJSON_Delete(registration);
}

static void expect_refused_launch(host_world *world, const char *tag, const char *expected_message, const char *expected_fix) {
    program launcher;
    const char *passed[] = {tag, "0", NULL};
    launch(world, &launcher, passed, NULL);
    CHECK(program_finish(&launcher, 30000) == 1);
    expect_contains(launcher.err, expected_message, "the plugin named");
    expect_contains(launcher.err, expected_fix, "with its fix");
    cJSON *never = application_start(world, tag);
    CHECK(never == NULL);
    CHECK(!exists_in(world->base.state, "processes/" BACKGROUND_ID ".json"));
    CHECK(!exists_in(world->base.state, "processes/" LOADER_ID ".json"));
    cJSON_Delete(never);
    program_free(&launcher);
}

static void starts_the_session_host_for_the_plugins_the_loader_requires(void) {
    host_world world;
    install_launch_world_requiring_session_plugin(&world, "launch", "required");
    world.layer_delay = "1000";
    program launcher;
    const char *passed[] = {"required", "0", NULL};
    launch(&world, &launcher, passed, NULL);
    cJSON *started = await_application(&world, "required");
    long session = await_holders(&world, LOADER_ID, 2);
    long background = await_holders(&world, BACKGROUND_ID, 2);
    char *starts = await_starts_holding(&world, "host.js " LOADER_ID);
    expect_contains(starts, "host.js " LOADER_ID, "session host started");
    release_application(&world, "required");
    CHECK(program_finish(&launcher, 30000) == 0);
    CHECK(gone_within(session, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    CHECK(gone_within(background, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    stop_if_alive(session);
    stop_if_alive(background);
    free(starts);
    cJSON_Delete(started);
    program_free(&launcher);
    finish_world(&world);
}

static void refuses_a_launch_naming_the_required_plugin_that_is_not_installed(void) {
    host_world world;
    install_launch_world_requiring_session_plugin(&world, "launch", "required-missing");
    char *folder = joined(world.base.cache, "bundles/" SESSION_PLUGIN);
    CHECK(runtime_remove_tree(folder) == 1);
    free(folder);
    expect_refused_launch(&world, "missing", SESSION_PLUGIN,
                          "forebay-host install --catalog <catalog> --application " APPLICATION_ID);
    finish_world(&world);
}

static void refuses_a_launch_naming_the_setting_of_a_required_plugin_that_is_disabled(void) {
    host_world world;
    install_launch_world_requiring_session_plugin(&world, "launch", "required-disabled");
    write_loader_setting(&world, "{\"plugins\": {\"" SESSION_PLUGIN "\": {\"enabled\": false}}}");
    cJSON *registration = registration_in(&world, LOADER_ID);
    char *file = joined(text_at(registration, "home"), "config/settings.json");
    char fix[2048];
    snprintf(fix, sizeof fix, "set plugins." SESSION_PLUGIN ".enabled to true in %s", file);
    expect_refused_launch(&world, "disabled", SESSION_PLUGIN, fix);
    free(file);
    cJSON_Delete(registration);
    finish_world(&world);
}

static void refuses_a_launch_when_the_session_host_of_a_loader_with_required_plugins_cannot_start(void) {
    host_world world;
    install_launch_world_requiring_session_plugin(&world, "launch", "required-no-javascript");
    char *record = joined(world.base.cache, "runtimes/javascript.json");
    char *node = joined(world.base.root, "node-bin/node" EXECUTABLE_ENDING);
    CHECK(runtime_remove_file_entry(record) == RUNTIME_ENTRY_DONE);
    CHECK(runtime_remove_file_entry(node) == RUNTIME_ENTRY_DONE);
    block_runtime_acquisition(&world);
    expect_refused_launch(&world, "blocked", "could not start the session host of " LOADER_ID, "fix: ");
    free(node);
    free(record);
    finish_world(&world);
}

static void starts_the_application_with_the_environment_its_session_host_publishes(void) {
    host_world world;
    install_launch_world(&world, "launch", "published", 1);
    world.fixture_ready = "{\"SPISOR_TEST\": \"1\"}";
    world.layer_delay = "500";
    program launcher;
    const char *passed[] = {"published", "0", NULL};
    launch(&world, &launcher, passed, NULL);
    cJSON *started = await_application(&world, "published");
    check_text(text_at(started, "spisorTest"), "1", "published variable seen by the application");
    long session = await_holders(&world, LOADER_ID, 2);
    long background = await_holders(&world, BACKGROUND_ID, 2);
    release_application(&world, "published");
    CHECK(program_finish(&launcher, 30000) == 0);
    CHECK(gone_within(session, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    CHECK(gone_within(background, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    stop_if_alive(session);
    stop_if_alive(background);
    cJSON_Delete(started);
    program_free(&launcher);
    finish_world(&world);
}

static void starts_the_application_unchanged_when_its_session_host_publishes_nothing(void) {
    host_world world;
    install_launch_world(&world, "launch", "nothing-published", 1);
    program launcher;
    const char *passed[] = {"unchanged", "0", NULL};
    launch(&world, &launcher, passed, NULL);
    cJSON *started = await_application(&world, "unchanged");
    CHECK(text_at(started, "spisorTest") == NULL);
    long session = await_holders(&world, LOADER_ID, 2);
    long background = await_holders(&world, BACKGROUND_ID, 2);
    release_application(&world, "unchanged");
    CHECK(program_finish(&launcher, 30000) == 0);
    CHECK(gone_within(session, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    CHECK(gone_within(background, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    stop_if_alive(session);
    stop_if_alive(background);
    cJSON_Delete(started);
    program_free(&launcher);
    finish_world(&world);
}

static void expect_unheld(const host_world *world, const char *id, const char *label) {
    cJSON *holders = holders_of(world, id);
    if (holder_count(holders) != 0) {
        fprintf(stderr, "%s: %s is still held\n", label, id);
        check_failures++;
    }
    cJSON_Delete(holders);
}

static void starts_the_application_with_the_environment_of_a_session_host_an_earlier_launch_left_running(void) {
    host_world world;
    install_launch_world(&world, "launch", "running-host", 1);
    world.fixture_ready = "{\"SPISOR_TEST\": \"first\"}";
    program first;
    const char *first_arguments[] = {"first", "0", NULL};
    launch(&world, &first, first_arguments, NULL);
    cJSON *first_started = await_application(&world, "first");
    long session = await_holders(&world, LOADER_ID, 2);
    world.fixture_ready = "none";
    world.ready_timeout = "1500";
    program second;
    const char *second_arguments[] = {"second", "0", NULL};
    launch(&world, &second, second_arguments, NULL);
    cJSON *second_started = await_application(&world, "second");
    check_text(text_at(second_started, "spisorTest"), "first", "environment the running host published earlier");
    CHECK(await_holders(&world, LOADER_ID, 4) == session);
    long background = await_holders(&world, BACKGROUND_ID, 4);
    release_application(&world, "first");
    release_application(&world, "second");
    CHECK(program_finish(&first, 30000) == 0);
    CHECK(program_finish(&second, 30000) == 0);
    CHECK(gone_within(session, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    stop_if_alive(session);
    stop_if_alive(background);
    cJSON_Delete(second_started);
    cJSON_Delete(first_started);
    program_free(&second);
    program_free(&first);
    finish_world(&world);
}

static void waits_past_a_ready_record_an_earlier_dead_session_host_left(void) {
    host_world world;
    install_launch_world(&world, "launch", "stale-record", 1);
    write_in(world.base.state, "processes/" LOADER_ID ".ready.json", "{\"pid\":999999,\"environment\":{\"SPISOR_TEST\":\"stale\"}}");
    world.fixture_ready = "{\"SPISOR_TEST\": \"fresh\"}";
    world.layer_delay = "1000";
    program launcher;
    const char *passed[] = {"stale", "0", NULL};
    launch(&world, &launcher, passed, NULL);
    cJSON *started = await_application(&world, "stale");
    check_text(text_at(started, "spisorTest"), "fresh", "record of the new host, not the stale one");
    long session = await_holders(&world, LOADER_ID, 2);
    long background = await_holders(&world, BACKGROUND_ID, 2);
    release_application(&world, "stale");
    CHECK(program_finish(&launcher, 30000) == 0);
    CHECK(gone_within(session, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    CHECK(gone_within(background, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    stop_if_alive(session);
    stop_if_alive(background);
    cJSON_Delete(started);
    program_free(&launcher);
    finish_world(&world);
}

static void refuses_the_launch_with_the_refusal_its_session_host_publishes(void) {
    host_world world;
    install_launch_world(&world, "launch", "refused", 1);
    world.fixture_ready = "refuse";
    program launcher;
    const char *passed[] = {"refused", "0", NULL};
    launch(&world, &launcher, passed, NULL);
    CHECK(program_finish(&launcher, 30000) == 1);
    expect_contains(launcher.err, "the fixture session host refused", "published refusal");
    expect_contains(launcher.err, "fix: the fixture fix", "published fix");
    expect_unheld(&world, LOADER_ID, "after a published refusal");
    expect_unheld(&world, BACKGROUND_ID, "after a published refusal");
    cJSON *never = application_start(&world, "refused");
    CHECK(never == NULL);
    cJSON_Delete(never);
    program_free(&launcher);
    finish_world(&world);
}

static void refuses_the_launch_when_its_session_host_never_becomes_ready(void) {
    host_world world;
    install_launch_world(&world, "launch", "never-ready", 1);
    world.fixture_ready = "none";
    world.ready_timeout = "300";
    program launcher;
    const char *passed[] = {"unready", "0", NULL};
    launch(&world, &launcher, passed, NULL);
    CHECK(program_finish(&launcher, 30000) == 1);
    expect_contains(launcher.err, "the session host of " LOADER_ID " did not become ready within 0.3 seconds", "timeout");
    expect_unheld(&world, LOADER_ID, "after a timeout");
    expect_unheld(&world, BACKGROUND_ID, "after a timeout");
    cJSON *never = application_start(&world, "unready");
    CHECK(never == NULL);
    cJSON_Delete(never);
    program_free(&launcher);
    finish_world(&world);
}

static void refuses_a_background_host_that_records_no_start_before_anything_starts(void) {
    host_world world;
    start_world_at(&world, "launch", "no-start");
    install_fake_application(&world);
    install_base *base = &world.base;
    list_native_forebay_host(base, "1.0.0");
    list_library(base, "runtime", "0.1.0");
    list_loader(base, LOADER_ID, "1.0.0", application_json(APPLICATION_ID, APPLICATION_ID, "{}"), "[]", 0,
                "[\"" BACKGROUND_ID "\"]");
    list_native_background(base, BACKGROUND_ID, 0);
    write_catalog(base);
    CHECK(world_install(&world, NULL, NULL) == 0);
    program launcher;
    const char *passed[] = {"never", "0", NULL};
    launch(&world, &launcher, passed, NULL);
    CHECK(program_finish(&launcher, 30000) == 1);
    expect_contains(launcher.err, "records no start", "no start");
    cJSON *never = application_start(&world, "never");
    CHECK(never == NULL);
    CHECK(!exists_in(world.base.state, "processes/" BACKGROUND_ID ".json"));
    cJSON_Delete(never);
    program_free(&launcher);
    finish_world(&world);
}

int main(void) {
    user_path_guard guard;
    guard_user_path(&guard);
    runs_the_application_with_its_arguments_answers_its_exit_code_and_the_background_host_stops();
    hands_the_application_every_argument_after_the_loader_id_a_leading_dash_dash_included();
    shares_one_background_process_between_two_launchers();
    keeps_the_application_and_its_background_host_after_a_hard_killed_launcher();
    refuses_an_application_that_is_not_on_the_path_leaving_nothing_held();
    runs_the_loader_version_its_references_schedule_keeping_the_one_it_ran_for_rollback();
#if !defined(_WIN32)
    passes_a_sigterm_on_to_the_application_and_answers_128_plus_its_number();
#endif
    starts_the_loader_s_session_host_on_javascript_host_when_its_settings_enable_plugins();
    starts_the_application_with_the_environment_its_session_host_publishes();
    starts_the_application_unchanged_when_its_session_host_publishes_nothing();
    starts_the_application_with_the_environment_of_a_session_host_an_earlier_launch_left_running();
    waits_past_a_ready_record_an_earlier_dead_session_host_left();
    refuses_the_launch_with_the_refusal_its_session_host_publishes();
    refuses_the_launch_when_its_session_host_never_becomes_ready();
    refuses_a_background_host_that_records_no_start_before_anything_starts();
    starts_the_session_host_on_a_javascript_runtime_found_again_when_the_recorded_one_is_gone();
    starts_the_session_host_on_its_bundle_s_sizes_without_hashing_it();
    runs_the_application_without_its_session_host_when_no_javascript_runtime_can_be_found();
    starts_the_session_host_for_the_plugins_the_loader_requires();
    refuses_a_launch_naming_the_required_plugin_that_is_not_installed();
    refuses_a_launch_naming_the_setting_of_a_required_plugin_that_is_disabled();
    refuses_a_launch_when_the_session_host_of_a_loader_with_required_plugins_cannot_start();
    runs_the_application_when_the_loader_cannot_be_registered_again_saying_why();
    expect_user_path_unchanged(&guard);
    return check_summary("launch_test");
}
