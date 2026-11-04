#include "host_fixture.h"

/* launch.test.ts */

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

/* design 7: a session host only when the loader's settings enable plugins of its own */

static void starts_the_loader_s_session_host_on_javascript_host_when_its_settings_enable_plugins(void) {
    host_world world;
    install_launch_world(&world, "launch", "session", 1);
    program launcher;
    const char *passed[] = {"session", "0", NULL};
    launch(&world, &launcher, passed, NULL);
    cJSON *started = await_application(&world, "session");
    long session = await_holders(&world, LOADER_ID, 2);
    long background = await_holders(&world, BACKGROUND_ID, 2);
    expect_holders(&world, LOADER_ID, launcher.child.pid, started_pid(started), "session held");
    char *starts = text_in(world.starts, "");
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

/* boot-executable.test.ts, as Ruling Z records the JS runtime */

static void refuses_a_session_host_when_install_recorded_no_javascript_runtime(void) {
    host_world world;
    install_launch_world(&world, "launch", "no-javascript", 1);
    char *record = joined(world.base.cache, "runtimes/javascript.json");
    CHECK(runtime_remove_file_entry(record) == RUNTIME_ENTRY_DONE);
    program launcher;
    const char *passed[] = {"unstarted", "0", NULL};
    launch(&world, &launcher, passed, NULL);
    CHECK(program_finish(&launcher, 30000) == 1);
    expect_contains(launcher.err, "no JavaScript runtime is recorded", "named");
    expect_contains(launcher.err, "fix: run forebay-host install --catalog <path or url> again", "reinstall fix");
    CHECK(!exists_in(world.base.state, "processes/" BACKGROUND_ID ".json"));
    free(record);
    program_free(&launcher);
    finish_world(&world);
}

/* Ruling S */

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
    refuses_a_background_host_that_records_no_start_before_anything_starts();
    refuses_a_session_host_when_install_recorded_no_javascript_runtime();
    expect_user_path_unchanged(&guard);
    return check_summary("launch_test");
}
