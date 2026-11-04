#include "host_fixture.h"

static char *stub_of(const host_world *world) {
    return joined(world->base.cache, "boot/forebay-boot" EXECUTABLE_ENDING);
}

static int run_boot(host_world *world, program *run, const char *const *arguments) {
    char *stub = stub_of(world);
    world_start(world, run, stub, arguments, NULL, 1);
    free(stub);
    return program_finish(run, 60000);
}

static void install_boot_world(host_world *world, const char *label) {
    install_launch_world(world, "boot-stub", label, 0);
    char *stub = stub_of(world);
    CHECK(spisor_file_exists(stub));
    free(stub);
}

/* boot-entry.test.ts and boot-host.ts, through forebay-boot */

static void runs_the_version_the_hint_names_with_its_arguments_passed_through(void) {
    host_world world;
    install_boot_world(&world, "hinted");
    program status;
    const char *status_arguments[] = {"forebay-host", "status", NULL};
    CHECK(run_boot(&world, &status, status_arguments) == 0);
    expect_contains(status.out, "\"hosts\"", "forebay-host status ran");
    program launcher;
    const char *launch_arguments[] = {"forebay-host", "launch", LOADER_ID, "booted", "5", "with space", NULL};
    char *stub = stub_of(&world);
    world_start(&world, &launcher, stub, launch_arguments, NULL, 0);
    cJSON *started = await_application(&world, "booted");
    expect_json(cJSON_GetObjectItemCaseSensitive(started, "argv"), "[\"booted\",\"5\",\"with space\"]", "passed through");
    long background = await_holders(&world, BACKGROUND_ID, 2);
    release_application(&world, "booted");
    CHECK(program_finish(&launcher, 30000) == 5);
    CHECK(gone_within(background, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    stop_if_alive(background);
    cJSON_Delete(started);
    free(stub);
    program_free(&launcher);
    program_free(&status);
    finish_world(&world);
}

static void refuses_a_swept_version_with_the_reinstall_fix(void) {
    host_world world;
    install_boot_world(&world, "swept");
    char *version = joined(world.base.cache, "bundles/forebay-host/1.0.0");
    CHECK(runtime_remove_tree(version) == 1);
    program run;
    const char *arguments[] = {"forebay-host", "status", NULL};
    CHECK(run_boot(&world, &run, arguments) == 1);
    expect_contains(run.err, "forebay-host is not installed in", "names what is missing");
    expect_contains(run.err, "fix: run forebay-host install --catalog <path or url> again", "reinstall fix");
    check_text(run.out, "", "ran nothing");
    free(version);
    program_free(&run);
    finish_world(&world);
}

static void refuses_with_the_reinstall_fix_when_there_is_no_hint_never_guessing_a_version(void) {
    host_world world;
    install_boot_world(&world, "no-hint");
    write_in(world.base.cache, "hosts/forebay-host.json", "{ not json");
    program run;
    const char *arguments[] = {"forebay-host", "status", NULL};
    CHECK(run_boot(&world, &run, arguments) == 1);
    expect_contains(run.err, "no host version is recorded for forebay-host", "no hint");
    expect_contains(run.err, "fix: run forebay-host install --catalog <path or url> again", "reinstall fix");
    program_free(&run);
    finish_world(&world);
}

static void refuses_a_bundle_whose_file_no_longer_has_its_hash(void) {
    host_world world;
    install_boot_world(&world, "damaged");
    char rest[256];
    snprintf(rest, sizeof rest, "bundles/forebay-host/1.0.0/%s/forebay-host" EXECUTABLE_ENDING, runtime_platform_key());
    write_in(world.base.cache, rest, "not the executable");
    program run;
    const char *arguments[] = {"forebay-host", "status", NULL};
    CHECK(run_boot(&world, &run, arguments) == 1);
    expect_contains(run.err, "forebay-host was not started", "damaged");
    program_free(&run);
    finish_world(&world);
}

static void refuses_a_start_that_names_no_host(void) {
    host_world world;
    install_boot_world(&world, "no-host");
    program run;
    const char *arguments[] = {NULL};
    CHECK(run_boot(&world, &run, arguments) == 1);
    expect_contains(run.err, "no host id was given", "no host id");
    expect_contains(run.err, "fix: pass the host id first", "its fix");
    program_free(&run);
    finish_world(&world);
}

/** What two updates leave while a launch runs: the hint names another version and no references name this one. */
static void update_twice_under_a_running_launch(host_world *world) {
    char *first = joined(world->base.cache, "bundles/forebay-host/1.0.0");
    char *second = joined(world->base.cache, "bundles/forebay-host/2.0.0");
    copy_tree(first, second);
    cJSON *hint = json_in(world->base.cache, "hosts/forebay-host.json");
    cJSON_ReplaceItemInObjectCaseSensitive(hint, "host", cJSON_CreateString("2.0.0"));
    char *text = cJSON_PrintUnformatted(hint);
    write_in(world->base.cache, "hosts/forebay-host.json", text);
    char *references = joined(world->base.state, "bundles/forebay-host.json");
    CHECK(runtime_remove_file_entry(references) == RUNTIME_ENTRY_DONE);
    free(references);
    cJSON_free(text);
    cJSON_Delete(hint);
    free(second);
    free(first);
}

static void keeps_the_forebay_host_version_a_launch_runs_through_a_sweep_until_it_ends(void) {
    host_world world;
    install_boot_world(&world, "own-run");
    program launcher;
    const char *arguments[] = {"forebay-host", "launch", LOADER_ID, "long", "0", NULL};
    char *stub = stub_of(&world);
    world_start(&world, &launcher, stub, arguments, NULL, 0);
    cJSON *started = await_application(&world, "long");
    long background = await_holders(&world, BACKGROUND_ID, 2);
    update_twice_under_a_running_launch(&world);
    sweep(&world);
    CHECK(exists_in(world.base.cache, "bundles/forebay-host/1.0.0"));
    release_application(&world, "long");
    CHECK(program_finish(&launcher, 30000) == 0);
    sweep(&world);
    CHECK(!exists_in(world.base.cache, "bundles/forebay-host/1.0.0"));
    CHECK(exists_in(world.base.cache, "bundles/forebay-host/2.0.0"));
    CHECK(gone_within(background, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    stop_if_alive(background);
    cJSON_Delete(started);
    free(stub);
    program_free(&launcher);
    finish_world(&world);
}

int main(void) {
    user_path_guard guard;
    guard_user_path(&guard);
    runs_the_version_the_hint_names_with_its_arguments_passed_through();
    refuses_a_swept_version_with_the_reinstall_fix();
    refuses_with_the_reinstall_fix_when_there_is_no_hint_never_guessing_a_version();
    refuses_a_bundle_whose_file_no_longer_has_its_hash();
    refuses_a_start_that_names_no_host();
    keeps_the_forebay_host_version_a_launch_runs_through_a_sweep_until_it_ends();
    expect_user_path_unchanged(&guard);
    return check_summary("boot_stub_test");
}
