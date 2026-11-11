#include "proof_fixture.h"

#include "boot_stub.h"

#define TIMED_RUNS 7
#define HOOK_BUDGET_MILLISECONDS 200.0
/**
 * The hook starts with a hidden console of its own, as Node's spawn with windowsHide starts one on Windows;
 * not detached, which would hand spisor-host a new console of its own and time that instead.
 */
#define DETACHED 0

static const char *const ONE_LOADER[] = {"1.0.0", NULL};

/** The command and arguments Claude Code runs at SessionStart, as the entry wrote them. */
typedef struct session_hook {
    char *command;
    const char *arguments[8];
    cJSON *written;
} session_hook;

static void read_session_hook(const host_world *world, session_hook *hook) {
    memset(hook, 0, sizeof *hook);
    hook->written = json_in(world->base.state, "entries/" LOADER_ID "/hooks/hooks.json");
    const cJSON *entry = cJSON_GetArrayItem(
        cJSON_GetObjectItemCaseSensitive(
            cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(hook->written, "hooks"), "SessionStart"), 0),
            "hooks"),
        0);
    hook->command = copy_text(text_at(entry, "command") == NULL ? "" : text_at(entry, "command"));
    size_t count = 0;
    const cJSON *argument;
    cJSON_ArrayForEach(argument, cJSON_GetObjectItemCaseSensitive(entry, "args")) {
        if (count < 7) {
            hook->arguments[count++] = argument->valuestring;
        }
    }
    hook->arguments[count] = NULL;
    CHECK(count == 4);
}

static void free_session_hook(session_hook *hook) {
    free(hook->command);
    cJSON_Delete(hook->written);
}

static void install_claude_code_loader(host_world *world) {
    start_world_at(world, "hook-without-javascript", "session-start");
    install_fake_application(world);
    prepare_user(world);
    build_proof_catalog(world, ONE_LOADER);
    char *index = index_path_of(&world->base);
    char *spisor_host = staged_spisor_host();
    const char *arguments[] = {"install", "--catalog", index, NULL};
    program run;
    CHECK(proof_run(world, &run, spisor_host, arguments, world->bin) == 0);
    program_free(&run);
    free(spisor_host);
    free(index);
}

/** Takes away what a SessionStart puts back: the run reference it records and the application its registration names. */
static void forget_what_a_start_records(const host_world *world) {
    char *references = joined(world->base.state, "bundles/" LOADER_ID ".json");
    CHECK(runtime_remove_file_entry(references) == RUNTIME_ENTRY_DONE);
    cJSON *registration = json_in(world->base.state, "hosts/" LOADER_ID ".json");
    CHECK(registration != NULL);
    cJSON_ReplaceItemInObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(registration, "application"), "label",
                                           cJSON_CreateString("a stale label"));
    char *text = cJSON_PrintUnformatted(registration);
    write_in(world->base.state, "hosts/" LOADER_ID ".json", text);
    cJSON_free(text);
    cJSON_Delete(registration);
    free(references);
}

static void expect_registered_and_recorded(const host_world *world, const program *run, const char *label) {
    check_text(run->out, "", label);
    check_text(run->err, "", label);
    cJSON *references = references_of(world, LOADER_ID);
    check_text(text_at(cJSON_GetObjectItemCaseSensitive(references, "run"), LOADER_ID), "1.0.0", label);
    cJSON *registration = registration_in(world, LOADER_ID);
    check_text(text_at(registration, "version"), "1.0.0", label);
    check_text(text_at(cJSON_GetObjectItemCaseSensitive(registration, "application"), "label"), "Claude Code", label);
    cJSON_Delete(registration);
    cJSON_Delete(references);
}

/** Runs the hook once on path; how long it took from start to exit, in milliseconds. */
static double run_session_start(host_world *world, const session_hook *hook, const char *path, const char *label) {
    forget_what_a_start_records(world);
    program run;
    double started = runtime_now_milliseconds();
    world_start(world, &run, hook->command, hook->arguments, path, DETACHED);
    int code = program_finish(&run, 30000);
    double took = runtime_now_milliseconds() - started;
    CHECK(code == 0);
    expect_registered_and_recorded(world, &run, label);
    program_free(&run);
    return took;
}

static int compare_milliseconds(const void *left, const void *right) {
    double difference = *(const double *)left - *(const double *)right;
    return (difference > 0) - (difference < 0);
}

/**
 * With every JavaScript runtime taken off the machine's PATH, the hook registers, records
 * and exits 0 within the budget; the median of several runs is held to it, so one slow process start on a
 * busy runner does not decide it.
 */
static void a_session_start_with_no_javascript_on_path_registers_records_and_finishes_within_its_budget(void) {
    host_world world;
    install_claude_code_loader(&world);
    session_hook hook;
    read_session_hook(&world, &hook);
    char *boot = runtime_boot_stub_path(world.base.cache);
    check_text(hook.command, boot, "the hook runs spisor-boot");
    char *machine_path = environment_value("PATH");
    char *path = path_without_javascript(machine_path);
    free(machine_path);
    expect_no_javascript_on(path, "the stripped PATH");
    double runs[TIMED_RUNS];
    for (int index = 0; index < TIMED_RUNS; index++) {
        runs[index] = run_session_start(&world, &hook, path, "a session start with no javascript");
    }
    qsort(runs, TIMED_RUNS, sizeof runs[0], compare_milliseconds);
    double median = runs[TIMED_RUNS / 2];
    printf("SessionStart hook on %s, %s build: median %.1f ms of %d runs, fastest %.1f ms, slowest %.1f ms; runs:", runtime_platform_key(),
           BUILD_TYPE, median, TIMED_RUNS, runs[0], runs[TIMED_RUNS - 1]);
    for (int index = 0; index < TIMED_RUNS; index++) {
        printf(" %.1f", runs[index]);
    }
    printf("\n");
    if (median > HOOK_BUDGET_MILLISECONDS) {
        fprintf(stderr, "the hook's median %.1f ms on %s, %s build, is over its %.0f ms budget; runs:", median, runtime_platform_key(),
                BUILD_TYPE, HOOK_BUDGET_MILLISECONDS);
        for (int index = 0; index < TIMED_RUNS; index++) {
            fprintf(stderr, " %.1f", runs[index]);
        }
        fprintf(stderr, "\n");
        check_failures++;
    }
    free(path);
    free(boot);
    free_session_hook(&hook);
    finish_world(&world);
}

/** A folder holding node and bun that only record being started. */
static char *javascript_trap_folder(const host_world *world) {
    char *folder = joined(world->base.root, "trap");
    for (size_t index = 0; index < sizeof JAVASCRIPT_RUNTIMES / sizeof JAVASCRIPT_RUNTIMES[0]; index++) {
        char name[64];
        snprintf(name, sizeof name, "%s" EXECUTABLE_ENDING, JAVASCRIPT_RUNTIMES[index]);
        char *path = joined(folder, name);
        copy_file(JAVASCRIPT_TRAP_PATH, path);
        free(path);
    }
    return folder;
}

/** Runs the hook once on path, the trap recording into started whatever runtime it is started as. */
static void run_with_trap(host_world *world, const session_hook *hook, const char *path, const char *started) {
    forget_what_a_start_records(world);
    char label[32];
    snprintf(label, sizeof label, "run-%d", ++world->runs);
    program run;
    prepare_program(&run, world->streams, label);
    cJSON *list = strings_of(hook->arguments);
    cJSON *environment = world_environment_with_path(world, path);
    cJSON_AddStringToObject(environment, "SPISOR_JAVASCRIPT_TRAP", started);
    program_start(&run, hook->command, list, environment, world->base.root, DETACHED);
    CHECK(program_finish(&run, 30000) == 0);
    expect_registered_and_recorded(world, &run, "a session start with node and bun on PATH");
    cJSON_Delete(environment);
    cJSON_Delete(list);
    program_free(&run);
}

/** With node and bun put back first on the PATH, the hook starts neither. */
static void a_session_start_starts_no_node_or_bun_even_when_one_is_on_path(void) {
    host_world world;
    install_claude_code_loader(&world);
    session_hook hook;
    read_session_hook(&world, &hook);
    char *trap = javascript_trap_folder(&world);
    char *machine_path = environment_value("PATH");
    char *stripped = path_without_javascript(machine_path);
    free(machine_path);
    char *path = malloc(strlen(trap) + strlen(stripped) + 2);
    sprintf(path, "%s" PATH_SEPARATOR "%s", trap, stripped);
    char *started = joined(world.base.root, "javascript-started.txt");
    run_with_trap(&world, &hook, path, started);
    if (spisor_file_exists(started)) {
        char *what = read_text_file(started);
        fprintf(stderr, "the hook started a JavaScript runtime: %s\n", what);
        free(what);
        check_failures++;
    }
    free(started);
    free(path);
    free(stripped);
    free(trap);
    free_session_hook(&hook);
    finish_world(&world);
}

/** A folder holding only a shell shim of node or bun, as npm and package managers put on PATH, is one the stripped PATH leaves out. */
static void a_shim_of_node_or_bun_counts_as_a_javascript_runtime(void) {
    host_world world;
    start_world_at(&world, "hook-without-javascript", "shims");
#if defined(_WIN32)
    static const char *const SHIMS[] = {"node.cmd", "bun.bat", "node.CMD"};
#else
    static const char *const SHIMS[] = {"node", "bun"};
#endif
    for (size_t index = 0; index < sizeof SHIMS / sizeof SHIMS[0]; index++) {
        char rest[64];
        snprintf(rest, sizeof rest, "shim-%zu/%s", index, SHIMS[index]);
        write_in(world.base.root, rest, "shim");
        snprintf(rest, sizeof rest, "shim-%zu", index);
        char *folder = joined(world.base.root, rest);
        if (!holds_javascript_runtime(folder)) {
            fprintf(stderr, "%s was not taken for a JavaScript runtime\n", SHIMS[index]);
            check_failures++;
        }
        free(folder);
    }
    CHECK(!holds_javascript_runtime(world.bin));
    finish_world(&world);
}

int main(void) {
    user_path_guard guard;
    guard_user_path(&guard);
    a_shim_of_node_or_bun_counts_as_a_javascript_runtime();
    a_session_start_with_no_javascript_on_path_registers_records_and_finishes_within_its_budget();
    a_session_start_starts_no_node_or_bun_even_when_one_is_on_path();
    expect_user_path_unchanged(&guard);
    return check_summary("hook_without_javascript");
}
