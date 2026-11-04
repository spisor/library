#include "host_fixture.h"

static int run_hook(host_world *world, program *run, const char *loader, const char *event) {
    const char *arguments[] = {"hook", loader, event, NULL};
    return world_run(world, run, arguments, NULL);
}

static int nothing_was_started(const host_world *world) {
    char *starts = text_in(world->starts, "");
    int none = starts == NULL || starts[0] == 0;
    free(starts);
    return none;
}

/* hook.test.ts */

static void registers_the_loader_and_records_its_reference_starting_nothing_and_printing_nothing(void) {
    host_world world;
    install_launch_world(&world, "hook", "records", 1);
    char *references_file = joined(world.base.state, "bundles/" LOADER_ID ".json");
    CHECK(runtime_remove_file_entry(references_file) == RUNTIME_ENTRY_DONE);
    free(references_file);
    program run;
    CHECK(run_hook(&world, &run, LOADER_ID, "session-start") == 0);
    check_text(run.out, "", "nothing on stdout");
    check_text(run.err, "", "nothing on stderr");
    cJSON *references = references_of(&world, LOADER_ID);
    check_text(text_at(cJSON_GetObjectItemCaseSensitive(references, "run"), LOADER_ID), "1.0.0", "reference recorded");
    CHECK(nothing_was_started(&world));
    CHECK(!exists_in(world.base.state, "processes/" BACKGROUND_ID ".json"));
    CHECK(!exists_in(world.base.state, "processes/" LOADER_ID ".json"));
    cJSON_Delete(references);
    program_free(&run);
    finish_world(&world);
}

static void registers_and_records_the_version_a_start_would_run_keeping_the_one_switched_from(void) {
    host_world world;
    install_launch_world(&world, "hook", "switch", 0);
    schedule_second_loader(&world);
    program run;
    CHECK(run_hook(&world, &run, LOADER_ID, "session-start") == 0);
    check_text(run.err, "", "silent switch");
    cJSON *references = references_of(&world, LOADER_ID);
    cJSON_DeleteItemFromObjectCaseSensitive(references, "unrecorded");
    expect_json(references,
                "{\"run\":{\"" LOADER_ID "\":\"2.0.0\"},\"next\":{},\"rollback\":{\"" LOADER_ID "\":\"1.0.0\"}}",
                "switched references");
    cJSON *registration = registration_in(&world, LOADER_ID);
    check_text(text_at(registration, "version"), "2.0.0", "registered at the switched version");
    sweep(&world);
    CHECK(exists_in(world.base.cache, "bundles/" LOADER_ID "/2.0.0"));
    CHECK(exists_in(world.base.cache, "bundles/" LOADER_ID "/1.0.0"));
    cJSON_Delete(registration);
    cJSON_Delete(references);
    program_free(&run);
    finish_world(&world);
}

static void writes_a_refusal_and_its_fix_to_stderr_and_still_exits_0(void) {
    host_world world;
    install_launch_world(&world, "hook", "refusals", 0);
    program unknown;
    CHECK(run_hook(&world, &unknown, LOADER_ID, "session-end") == 0);
    check_text(unknown.out, "", "unknown event stdout");
    expect_contains(unknown.err, "the hook event \"session-end\" is not one this host answers", "unknown event");
    expect_contains(unknown.err, "fix: run the hook with one of: session-start", "unknown event fix");
    program unregistered;
    CHECK(run_hook(&world, &unregistered, "absent-loader", "session-start") == 0);
    check_text(unregistered.out, "", "unregistered stdout");
    expect_contains(unregistered.err, "absent-loader is not registered", "unregistered");
    expect_contains(unregistered.err, "fix: ", "unregistered fix");
    CHECK(nothing_was_started(&world));
    program_free(&unregistered);
    program_free(&unknown);
    finish_world(&world);
}

/* launch.test.ts, the provenance case */

static cJSON *recorded_provenance(const host_world *world) {
    cJSON *registration = registration_in(world, LOADER_ID);
    cJSON *provenance = cJSON_DetachItemFromObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(registration, "application"),
                                                               "provenance");
    cJSON_Delete(registration);
    return provenance;
}

static void keeps_the_application_s_provenance_across_a_launch_and_a_hook(void) {
    host_world world;
    start_world_at(&world, "hook", "provenance");
    list_launch_catalog(&world, "[]", 0);
    const char *choice[] = {"--application", APPLICATION_ID "=fake", NULL};
    CHECK(world_install(&world, choice, NULL) == 0);
    cJSON *installed = recorded_provenance(&world);
    check_text(text_at(installed, "method"), "fake", "installed through fake");
    check_text(text_at(installed, "package"), "fixture-pkg", "its package");
    install_fake_application(&world);
    program launcher;
    const char *passed[] = {"provenance", "0", NULL};
    launch(&world, &launcher, passed, NULL);
    cJSON *started = await_application(&world, "provenance");
    long background = await_holders(&world, BACKGROUND_ID, 2);
    release_application(&world, "provenance");
    CHECK(program_finish(&launcher, 30000) == 0);
    program hook;
    CHECK(run_hook(&world, &hook, LOADER_ID, "session-start") == 0);
    cJSON *kept = recorded_provenance(&world);
    CHECK(installed != NULL && kept != NULL && cJSON_Compare(installed, kept, 1));
    CHECK(gone_within(background, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    stop_if_alive(background);
    cJSON_Delete(kept);
    cJSON_Delete(started);
    cJSON_Delete(installed);
    program_free(&hook);
    program_free(&launcher);
    finish_world(&world);
}

static void keeps_the_version_a_launch_switched_to_when_a_later_hook_records_the_loader(void) {
    host_world world;
    install_launch_world(&world, "hook", "after-launch", 0);
    schedule_second_loader(&world);
    program launcher;
    const char *passed[] = {"switched", "0", NULL};
    launch(&world, &launcher, passed, NULL);
    cJSON *started = await_application(&world, "switched");
    long background = await_holders(&world, BACKGROUND_ID, 2);
    release_application(&world, "switched");
    CHECK(program_finish(&launcher, 30000) == 0);
    program hook;
    CHECK(run_hook(&world, &hook, LOADER_ID, "session-start") == 0);
    check_text(hook.out, "", "silent");
    check_text(hook.err, "", "nothing refused");
    cJSON *references = references_of(&world, LOADER_ID);
    check_text(text_at(cJSON_GetObjectItemCaseSensitive(references, "run"), LOADER_ID), "2.0.0", "still the switched version");
    cJSON *registration = registration_in(&world, LOADER_ID);
    check_text(text_at(registration, "version"), "2.0.0", "registered at it");
    sweep(&world);
    CHECK(exists_in(world.base.cache, "bundles/" LOADER_ID "/2.0.0"));
    CHECK(gone_within(background, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    stop_if_alive(background);
    cJSON_Delete(registration);
    cJSON_Delete(references);
    cJSON_Delete(started);
    program_free(&hook);
    program_free(&launcher);
    finish_world(&world);
}

int main(void) {
    user_path_guard guard;
    guard_user_path(&guard);
    registers_the_loader_and_records_its_reference_starting_nothing_and_printing_nothing();
    registers_and_records_the_version_a_start_would_run_keeping_the_one_switched_from();
    writes_a_refusal_and_its_fix_to_stderr_and_still_exits_0();
    keeps_the_application_s_provenance_across_a_launch_and_a_hook();
    keeps_the_version_a_launch_switched_to_when_a_later_hook_records_the_loader();
    expect_user_path_unchanged(&guard);
    return check_summary("hook_test");
}
