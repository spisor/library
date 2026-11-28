#include "host_fixture.h"
#include "host_output.h"
#include "spisor/allocation.h"

#include "holders.h"
#include "processes.h"
#include "processes_holders.h"
#include "runtime/processes.h"

static char *stub_of(const host_world *world) {
    return joined(world->base.cache, "boot/spisor-boot" EXECUTABLE_ENDING);
}

static int run_boot(host_world *world, program *run, const char *const *arguments) {
    char *stub = stub_of(world);
    world_start(world, run, stub, arguments, NULL, 1);
    spisor_memory_release(stub);
    return program_finish(run, 60000);
}

static void install_boot_world(host_world *world, const char *label) {
    install_launch_world(world, "boot-stub", label, 0);
    char *stub = stub_of(world);
    CHECK(spisor_probe_file(stub) == SPISOR_FILE_PRESENT);
    spisor_memory_release(stub);
}

static void runs_the_version_the_hint_names_with_its_arguments_passed_through(void) {
    host_world world;
    install_boot_world(&world, "hinted");
    program status;
    const char *status_arguments[] = {"spisor-host", "status", NULL};
    CHECK(run_boot(&world, &status, status_arguments) == 0);
    expect_contains(status.out, "\"hosts\"", "spisor-host status ran");
    program launcher;
    const char *launch_arguments[] = {"spisor-host", "launch", LOADER_ID, "booted", "5", "with space", NULL};
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
    spisor_memory_release(stub);
    program_free(&launcher);
    program_free(&status);
    finish_world(&world);
}

static void refuses_a_swept_version_with_the_reinstall_fix(void) {
    host_world world;
    install_boot_world(&world, "swept");
    char *version = joined(world.base.cache, "bundles/spisor-host/1.0.0");
    CHECK(runtime_remove_tree(version) == SPISOR_OK);
    program run;
    const char *arguments[] = {"spisor-host", "status", NULL};
    CHECK(run_boot(&world, &run, arguments) == 1);
    expect_contains(run.err, "spisor-host is not installed in", "names what is missing");
    expect_contains(run.err, "fix: run spisor-host install --catalog <path or url> again", "reinstall fix");
    check_text(run.out, "", "ran nothing");
    spisor_memory_release(version);
    program_free(&run);
    finish_world(&world);
}

static void refuses_with_the_reinstall_fix_when_there_is_no_hint_never_guessing_a_version(void) {
    host_world world;
    install_boot_world(&world, "no-hint");
    write_in(world.base.cache, "hosts/spisor-host.json", "{ not json");
    program run;
    const char *arguments[] = {"spisor-host", "status", NULL};
    CHECK(run_boot(&world, &run, arguments) == 1);
    expect_contains(run.err, "no host version is recorded for spisor-host", "no hint");
    expect_contains(run.err, "fix: run spisor-host install --catalog <path or url> again", "reinstall fix");
    program_free(&run);
    finish_world(&world);
}

static void refuses_a_bundle_whose_file_no_longer_has_its_recorded_size(void) {
    host_world world;
    install_boot_world(&world, "damaged");
    char rest[256];
    snprintf(rest, sizeof rest, "bundles/spisor-host/1.0.0/%s/spisor-host" EXECUTABLE_ENDING, runtime_platform_key());
    write_in(world.base.cache, rest, "not the executable");
    program run;
    const char *arguments[] = {"spisor-host", "status", NULL};
    CHECK(run_boot(&world, &run, arguments) == 1);
    expect_contains(run.err, "spisor-host was not started", "damaged");
    expect_contains(run.err, "but its install recorded", "names the size");
    expect_contains(run.err, "fix: run spisor-host install --catalog <path or url> again", "reinstall fix");
    program_free(&run);
    finish_world(&world);
}

/** The carried spisor-boot copy with its last byte flipped: the start's size check passes, status's hashes do not. */
static void starts_a_same_size_corruption_which_status_then_refuses(void) {
    host_world world;
    install_boot_world(&world, "same-size");
    char rest[256];
    snprintf(rest, sizeof rest, "bundles/spisor-host/1.0.0/%s/spisor-boot" EXECUTABLE_ENDING, runtime_platform_key());
    char *carried = joined(world.base.cache, rest);
    size_t length = 0;
    uint8_t *bytes = spisor_read_file_reporting(carried, &length, &(spisor_read_outcome){SPISOR_READ_FAILED});
    CHECK(bytes != NULL && length > 0);
    if (bytes != NULL && length > 0) {
        bytes[length - 1] ^= 0xff;
        CHECK(spisor_replace_file(carried, bytes, length) == SPISOR_OK);
    }
    program run;
    const char *arguments[] = {"spisor-host", "status", NULL};
    CHECK(run_boot(&world, &run, arguments) == 1);
    expect_contains(run.out, "\"damaged\"", "status ran and lists what is damaged");
    expect_contains(run.err, "spisor-boot" EXECUTABLE_ENDING, "names the file");
    expect_contains(run.err, "has SHA-256", "found by its hash");
    expect_contains(run.err, "fix: ", "with a fix");
    spisor_memory_release(bytes);
    spisor_memory_release(carried);
    program_free(&run);
    finish_world(&world);
}

static void exits_with_the_code_refused_exit_names_for_its_own_refusal_and_for_what_it_boots(void) {
    host_world world;
    install_boot_world(&world, "refused-exit");
    program run;
    const char *booted[] = {"spisor-host", "--refused-exit", "3", "ui", LOADER_ID, "--window", NULL};
    CHECK(run_boot(&world, &run, booted) == 3);
    expect_contains(run.err, "interface", "the booted host's refusal is printed");
    program_free(&run);
    write_in(world.base.cache, "hosts/spisor-host.json", "{ not json");
    const char *own[] = {"spisor-host", "--refused-exit", "3", "status", NULL};
    CHECK(run_boot(&world, &run, own) == 3);
    expect_contains(run.err, "no host version is recorded for spisor-host", "the stub's own refusal");
    program_free(&run);
    const char *out_of_range[] = {"spisor-host", "--refused-exit", "256", "status", NULL};
    CHECK(run_boot(&world, &run, out_of_range) == HOST_EXIT_MISUSED);
    expect_contains(run.err, "--refused-exit takes a code from 1 to 255", "names the range");
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
    char *first = joined(world->base.cache, "bundles/spisor-host/1.0.0");
    char *second = joined(world->base.cache, "bundles/spisor-host/2.0.0");
    copy_tree(first, second);
    cJSON *hint = json_in(world->base.cache, "hosts/spisor-host.json");
    cJSON_ReplaceItemInObjectCaseSensitive(hint, "host", cJSON_CreateString("2.0.0"));
    char *text = cJSON_PrintUnformatted(hint);
    write_in(world->base.cache, "hosts/spisor-host.json", text);
    char *references = joined(world->base.state, "bundles/spisor-host.json");
    CHECK(runtime_remove_file_entry(references) == RUNTIME_ENTRY_DONE);
    spisor_memory_release(references);
    cJSON_free(text);
    cJSON_Delete(hint);
    spisor_memory_release(second);
    spisor_memory_release(first);
}

static void keeps_the_spisor_host_version_a_launch_runs_through_a_sweep_until_it_ends(void) {
    host_world world;
    install_boot_world(&world, "own-run");
    program launcher;
    const char *arguments[] = {"spisor-host", "launch", LOADER_ID, "long", "0", NULL};
    char *stub = stub_of(&world);
    world_start(&world, &launcher, stub, arguments, NULL, 0);
    cJSON *started = await_application(&world, "long");
    long background = await_holders(&world, BACKGROUND_ID, 2);
    update_twice_under_a_running_launch(&world);
    sweep(&world);
    CHECK(exists_in(world.base.cache, "bundles/spisor-host/1.0.0"));
    release_application(&world, "long");
    CHECK(program_finish(&launcher, 30000) == 0);
    sweep(&world);
    CHECK(!exists_in(world.base.cache, "bundles/spisor-host/1.0.0"));
    CHECK(exists_in(world.base.cache, "bundles/spisor-host/2.0.0"));
    CHECK(gone_within(background, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    stop_if_alive(background);
    cJSON_Delete(started);
    spisor_memory_release(stub);
    program_free(&launcher);
    finish_world(&world);
}

#if defined(_WIN32)
static int runs_version(const runtime_processes_run *run, const char *id, const char *version) {
    for (size_t index = 0; index < run->version_count; index++) {
        if (strcmp(run->versions[index].id, id) == 0 && strcmp(run->versions[index].version, version) == 0) {
            return 1;
        }
    }
    return 0;
}

/** Whether a live run that processes.liveRuns answers runs spisor-host 1.0.0. */
static int records_spisor_host(const void *context) {
    const host_world *world = context;
    const runtime_processes_definition *processes = runtime_processes_definition_of();
    runtime_processes_live_argument argument = SPISOR_INIT(runtime_processes_live_argument);
    runtime_processes_live_answer answer = SPISOR_INIT(runtime_processes_live_answer);
    int recorded = 0;
    argument.state = world->base.state;
    if (processes->live_runs(processes->context, &argument, &answer, NULL) == SPISOR_OK) {
        for (size_t index = 0; !recorded && index < answer.run_count; index++) {
            recorded = runs_version(&answer.runs[index], "spisor-host", "1.0.0");
        }
    }
    processes->release_live_answer(processes->context, &answer);
    return recorded;
}

static int records_no_spisor_host(const void *context) {
    return !records_spisor_host(context);
}

/** Holds the background host for this test's own pid, with no host process recorded yet, as a launcher would. */
static void hold_for_this_test(const host_world *world) {
    char text[256];
    snprintf(text, sizeof text, "{\"pid\":null,\"aliveAt\":null,\"checkMilliseconds\":%d,\"holders\":[%d]}",
             INTERVAL_MILLISECONDS, (int)fixture_process_id());
    write_in(world->base.state, "processes/" BACKGROUND_ID ".json", text);
}

static cJSON *drop_this_test(const void *context, const char *path, runtime_holders_record *record, cJSON **refusal) {
    (void)context;
    (void)refusal;
    runtime_keep_live_holders(record, (double)fixture_process_id());
    return runtime_write_holders(path, record) == SPISOR_OK ? cJSON_CreateObject() : NULL;
}

/** Releases this test's hold as a launcher does: under the holders lock, keeping what the host recorded. */
static void release_this_test(const host_world *world) {
    cJSON *refusal = NULL;
    cJSON *answer = runtime_processes_with_holders(world->base.state, BACKGROUND_ID, runtime_now_milliseconds(),
                                                   drop_this_test, NULL, &refusal);
    CHECK(answer != NULL && refusal == NULL);
    cJSON_Delete(answer);
    cJSON_Delete(refusal);
}

static void records_the_spisor_host_version_a_background_command_runs_while_its_layer_runs(void) {
    host_world world;
    install_boot_world(&world, "background-run");
    hold_for_this_test(&world);
    program background;
    const char *arguments[] = {"spisor-host", "background", BACKGROUND_ID, NULL};
    char *stub = stub_of(&world);
    world_start(&world, &background, stub, arguments, NULL, 0);
    CHECK(wait_until(records_spisor_host, &world, START_TIMEOUT));
    release_this_test(&world);
    CHECK(program_finish(&background, 30000) == 0);
    CHECK(wait_until(records_no_spisor_host, &world, 5000));
    spisor_memory_release(stub);
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
    char *narrow = spisor_memory_allocate(4 * (size_t)length + 1);
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
    spisor_memory_release(world.base.cache);
    world.base.cache = short_cache;
    program launcher;
    const char *arguments[] = {"spisor-host", "launch", LOADER_ID, "short", "0", NULL};
    world_start(&world, &launcher, stub, arguments, NULL, 0);
    cJSON *started = await_application(&world, "short");
    CHECK(records_spisor_host(&world));
    long background = await_holders(&world, BACKGROUND_ID, 2);
    release_application(&world, "short");
    CHECK(program_finish(&launcher, 30000) == 0);
    CHECK(gone_within(background, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    stop_if_alive(background);
    cJSON_Delete(started);
    spisor_memory_release(stub);
    program_free(&launcher);
    finish_world(&world);
}
#endif

#if defined(_WIN32)
/**
 * spisor-boot started with no console at all, as an application that spawns its hooks detached starts
 * it: the spisor-host it boots gets a console with no window, which the application it launches
 * inherits, rather than a console window of its own.
 */
static void gives_what_it_boots_no_console_window_when_it_has_no_console(void) {
    host_world world;
    install_boot_world(&world, "no-console");
    program launcher;
    const char *arguments[] = {"spisor-host", "launch", LOADER_ID, "windowless", "0", NULL};
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
    spisor_memory_release(stub);
    program_free(&launcher);
    finish_world(&world);
}

static int lists_pid(const cJSON *pids, long pid) {
    const cJSON *entry;
    cJSON_ArrayForEach(entry, pids) {
        if (cJSON_IsNumber(entry) && (long)entry->valuedouble == pid) {
            return 1;
        }
    }
    return 0;
}

/**
 * spisor-boot started in a console with no window, as Node's spawn with windowsHide starts a hook: what it
 * boots shares that console rather than getting one of its own, so the application spisor-host launches,
 * which inherits spisor-host's, is attached to spisor-boot's console.
 */
static void shares_a_windowless_console_with_what_it_boots(void) {
    host_world world;
    install_boot_world(&world, "hidden-console");
    program launcher;
    const char *arguments[] = {"spisor-host", "launch", LOADER_ID, "shared", "0", NULL};
    char *stub = stub_of(&world);
    world_start(&world, &launcher, stub, arguments, NULL, 0);
    cJSON *started = await_application(&world, "shared");
    const cJSON *pids = cJSON_GetObjectItemCaseSensitive(started, "consoleProcesses");
    if (!lists_pid(pids, launcher.child.pid)) {
        char *printed = cJSON_PrintUnformatted(pids);
        fprintf(stderr, "spisor-boot %ld is not attached to the application's console %s\n", launcher.child.pid, printed);
        cJSON_free(printed);
        check_failures++;
    }
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(started, "consoleWindow")));
    long background = await_holders(&world, BACKGROUND_ID, 2);
    release_application(&world, "shared");
    CHECK(program_finish(&launcher, 30000) == 0);
    CHECK(gone_within(background, INTERVAL_MILLISECONDS * 2 + STOP_ALLOWANCE));
    stop_if_alive(background);
    cJSON_Delete(started);
    spisor_memory_release(stub);
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
    exits_with_the_code_refused_exit_names_for_its_own_refusal_and_for_what_it_boots();
    keeps_the_spisor_host_version_a_launch_runs_through_a_sweep_until_it_ends();
#if defined(_WIN32)
    records_the_spisor_host_version_a_background_command_runs_while_its_layer_runs();
    identifies_the_running_version_when_the_cache_is_named_by_its_short_name();
    gives_what_it_boots_no_console_window_when_it_has_no_console();
    shares_a_windowless_console_with_what_it_boots();
#endif
    expect_user_path_unchanged(&guard);
    return check_summary("boot_stub_test");
}
