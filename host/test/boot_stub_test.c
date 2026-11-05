#include "host_fixture.h"

#include "processes.h"

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

static void refuses_a_bundle_whose_file_no_longer_has_its_recorded_size(void) {
    host_world world;
    install_boot_world(&world, "damaged");
    char rest[256];
    snprintf(rest, sizeof rest, "bundles/forebay-host/1.0.0/%s/forebay-host" EXECUTABLE_ENDING, runtime_platform_key());
    write_in(world.base.cache, rest, "not the executable");
    program run;
    const char *arguments[] = {"forebay-host", "status", NULL};
    CHECK(run_boot(&world, &run, arguments) == 1);
    expect_contains(run.err, "forebay-host was not started", "damaged");
    expect_contains(run.err, "but its install recorded", "names the size");
    expect_contains(run.err, "fix: run forebay-host install --catalog <path or url> again", "reinstall fix");
    program_free(&run);
    finish_world(&world);
}

/** The carried forebay-boot copy with its last byte flipped: the start's size check passes, status's hashes do not. */
static void starts_a_same_size_corruption_which_status_then_refuses(void) {
    host_world world;
    install_boot_world(&world, "same-size");
    char rest[256];
    snprintf(rest, sizeof rest, "bundles/forebay-host/1.0.0/%s/forebay-boot" EXECUTABLE_ENDING, runtime_platform_key());
    char *carried = joined(world.base.cache, rest);
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(carried, &length);
    CHECK(bytes != NULL && length > 0);
    if (bytes != NULL && length > 0) {
        bytes[length - 1] ^= 0xff;
        CHECK(spisor_replace_file(carried, bytes, length) == 1);
    }
    program run;
    const char *arguments[] = {"forebay-host", "status", NULL};
    CHECK(run_boot(&world, &run, arguments) == 1);
    expect_contains(run.out, "\"damaged\"", "status ran and lists what is damaged");
    expect_contains(run.err, "forebay-boot" EXECUTABLE_ENDING, "names the file");
    expect_contains(run.err, "has SHA-256", "found by its hash");
    expect_contains(run.err, "fix: ", "with a fix");
    free(bytes);
    free(carried);
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

#if defined(_WIN32)
/** The live runs processes.liveRuns answers, as one JSON text. */
static char *live_runs(const host_world *world) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "state", world->base.state);
    catalog_answer answer = call_catalog(runtime_processes_live_runs, argument);
    char *printed = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(answer.tree, "runs"));
    cJSON_Delete(answer.tree);
    return printed;
}

static int records_forebay_host(const void *context) {
    char *runs = live_runs(context);
    int recorded = runs != NULL && strstr(runs, "\"forebay-host\":\"1.0.0\"") != NULL;
    cJSON_free(runs);
    return recorded;
}

static int records_no_forebay_host(const void *context) {
    return !records_forebay_host(context);
}

/** Holds the background host for this test's own pid, with no host process recorded yet, as a launcher would. */
static void hold_for_this_test(const host_world *world, int held) {
    char text[256];
    snprintf(text, sizeof text, "{\"pid\":null,\"aliveAt\":null,\"checkMilliseconds\":%d,\"holders\":[%.0d]}",
             INTERVAL_MILLISECONDS, held ? (int)fixture_process_id() : 0);
    write_in(world->base.state, "processes/" BACKGROUND_ID ".json", text);
}

static void records_the_forebay_host_version_a_background_command_runs_while_its_layer_runs(void) {
    host_world world;
    install_boot_world(&world, "background-run");
    hold_for_this_test(&world, 1);
    program background;
    const char *arguments[] = {"forebay-host", "background", BACKGROUND_ID, NULL};
    char *stub = stub_of(&world);
    world_start(&world, &background, stub, arguments, NULL, 0);
    CHECK(wait_until(records_forebay_host, &world, START_TIMEOUT));
    hold_for_this_test(&world, 0);
    CHECK(program_finish(&background, 30000) == 0);
    CHECK(wait_until(records_no_forebay_host, &world, 5000));
    free(stub);
    program_free(&background);
    finish_world(&world);
}

/** The 8.3 name of path, or NULL when the volume keeps none or it equals path. */
static char *short_name_of(const char *path) {
    wchar_t wide[1024];
    wchar_t short_wide[1024];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 1024);
    DWORD length = GetShortPathNameW(wide, short_wide, 1024);
    if (length == 0 || length >= 1024 || wcscmp(wide, short_wide) == 0) {
        return NULL;
    }
    char *narrow = malloc(4 * (size_t)length + 1);
    WideCharToMultiByte(CP_UTF8, 0, short_wide, -1, narrow, 4 * (int)length + 1, NULL, NULL);
    return narrow;
}

/** The stub runs by its long name while SPISOR_CACHE names the cache by its 8.3 one. */
static void identifies_the_running_version_when_the_cache_is_named_by_its_short_name(void) {
    host_world world;
    install_boot_world(&world, "short-name-cache");
    char *short_cache = short_name_of(world.base.cache);
    if (short_cache == NULL) {
        fprintf(stderr, "this volume keeps no 8.3 names; the short-name case was not run\n");
        finish_world(&world);
        return;
    }
    char *stub = stub_of(&world);
    free(world.base.cache);
    world.base.cache = short_cache;
    program launcher;
    const char *arguments[] = {"forebay-host", "launch", LOADER_ID, "short", "0", NULL};
    world_start(&world, &launcher, stub, arguments, NULL, 0);
    cJSON *started = await_application(&world, "short");
    CHECK(records_forebay_host(&world));
    long background = await_holders(&world, BACKGROUND_ID, 2);
    release_application(&world, "short");
    CHECK(program_finish(&launcher, 30000) == 0);
    CHECK(gone_within(background, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    stop_if_alive(background);
    cJSON_Delete(started);
    free(stub);
    program_free(&launcher);
    finish_world(&world);
}
#endif

#if defined(_WIN32)
/**
 * forebay-boot started with no console at all, as an application that spawns its hooks detached starts
 * it: the forebay-host it boots gets a console with no window, which the application it launches
 * inherits, rather than a console window of its own.
 */
static void gives_what_it_boots_no_console_window_when_it_has_no_console(void) {
    host_world world;
    install_boot_world(&world, "no-console");
    program launcher;
    const char *arguments[] = {"forebay-host", "launch", LOADER_ID, "windowless", "0", NULL};
    char *stub = stub_of(&world);
    world_start(&world, &launcher, stub, arguments, NULL, 1);
    cJSON *started = await_application(&world, "windowless");
    const cJSON *window = cJSON_GetObjectItemCaseSensitive(started, "consoleWindow");
    CHECK(cJSON_IsFalse(window));
    long background = await_holders(&world, BACKGROUND_ID, 2);
    release_application(&world, "windowless");
    CHECK(program_finish(&launcher, 30000) == 0);
    CHECK(gone_within(background, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    stop_if_alive(background);
    cJSON_Delete(started);
    free(stub);
    program_free(&launcher);
    finish_world(&world);
}
#endif

int main(void) {
    user_path_guard guard;
    guard_user_path(&guard);
    runs_the_version_the_hint_names_with_its_arguments_passed_through();
    refuses_a_swept_version_with_the_reinstall_fix();
    refuses_with_the_reinstall_fix_when_there_is_no_hint_never_guessing_a_version();
    refuses_a_bundle_whose_file_no_longer_has_its_recorded_size();
    starts_a_same_size_corruption_which_status_then_refuses();
    refuses_a_start_that_names_no_host();
    keeps_the_forebay_host_version_a_launch_runs_through_a_sweep_until_it_ends();
#if defined(_WIN32)
    records_the_forebay_host_version_a_background_command_runs_while_its_layer_runs();
    identifies_the_running_version_when_the_cache_is_named_by_its_short_name();
    gives_what_it_boots_no_console_window_when_it_has_no_console();
#endif
    expect_user_path_unchanged(&guard);
    return check_summary("boot_stub_test");
}
