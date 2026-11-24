#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "allocation.h"
#include "allocation_failure.h"
#include "host_fixture.h"
#include "spisor/json_allocation.h"

/**
 * @file
 * Fails every allocation of `weivel.host`'s start and of the stop after it, one at a time and then from one on, and
 * requires each start to answer OUT_OF_MEMORY with its plan and refusal empty, nothing left allocated, no run record
 * and no run left to stop, or to answer as a clean run does. Then fails every allocation of the calls after a clean
 * start, requiring each call to answer OUT_OF_MEMORY, failing its run, or to answer as a clean run does, and a tick
 * that answers OK to have installed the newer plugin its update check finds, so a check that ran out of memory is
 * never answered as one that ran.
 */

#define PRINT_CAPACITY 4096

typedef struct start_case {
    const char *label;
    const char *const *arguments;
    size_t argument_count;
    /** What a clean run answers. */
    weivel_status expected;
} start_case;

typedef struct clean_run {
    spisor_status status;
    char printed[PRINT_CAPACITY];
    uint64_t allocations;
} clean_run;

static char printed[PRINT_CAPACITY];

static void put(const char *text) {
    size_t length = strlen(printed);
    snprintf(printed + length, sizeof printed - length, "%s;", text == NULL ? "(none)" : text);
}

static void put_plan(const weivel_standard_host_plan *plan) {
    char counts[128];
    snprintf(counts, sizeof counts, "%u:%zu:%zu:%u:%zu", (unsigned)plan->mode, plan->library_count,
             plan->plugin_count, (unsigned)plan->action_arguments.kind, plan->action_arguments.count);
    put(counts);
    for (size_t index = 0; index < plan->library_count; index++) {
        put(plan->libraries[index].binary);
        put(plan->libraries[index].declaration);
    }
    for (size_t index = 0; index < plan->plugin_count; index++) {
        put(plan->plugins[index].manifest);
        put(plan->plugins[index].entry);
    }
    put(plan->action_plugin);
    put(plan->action_name);
    put(plan->action_service);
    put(plan->action_method);
}

/** Starts the case's run, and stops it when the start answered OK, so every run ends with no run started. */
static spisor_status run_start(const start_case *each) {
    const weivel_standard_host *host = host_definition();
    weivel_standard_host_plan plan;
    weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
    uint32_t exit_code = 0;
    spisor_status status;
    start_empty(&plan, sizeof plan);
    status = (spisor_status)host->start(host->context, each->arguments, each->argument_count, &plan, &refusal);
    put_plan(&plan);
    put(refusal.detail);
    status = empty_unless_answered(status, &plan, sizeof plan);
    host->release_plan(host->context, &plan);
    host->release_refusal(host->context, &refusal);
    if (status == SPISOR_OK && host->stop(host->context, &exit_code, NULL) != WEIVEL_STATUS_OK) {
        put("the stop failed");
    }
    return status;
}

/** Whether a run is still started, which only a start that answered OK without its stop leaves. */
static int a_run_is_left(void) {
    const weivel_standard_host *host = host_definition();
    weivel_standard_host_next next = WEIVEL_INIT(weivel_standard_host_next);
    weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
    weivel_status status = host->tick(host->context, 0, &next, &refusal);
    int left = status != WEIVEL_STATUS_REFUSED || refusal.detail == NULL || strstr(refusal.detail, "no run") == NULL;
    host->release_next(host->context, &next);
    host->release_refusal(host->context, &refusal);
    return left;
}

static allocation_failure_mode failure_mode = FAIL_ONE_ALLOCATION;

static int kept_nothing(const start_case *each, uint64_t attempt, int64_t baseline) {
    if (spisor_memory_live_blocks() != baseline) {
        fprintf(stderr, "%s: failing allocation %llu left %lld blocks live\n", each->label,
                (unsigned long long)attempt, (long long)(spisor_memory_live_blocks() - baseline));
        return 0;
    }
    if (run_record_count() != 0 || a_run_is_left()) {
        fprintf(stderr, "%s: failing allocation %llu left a run record or a run\n", each->label,
                (unsigned long long)attempt);
        return 0;
    }
    return 1;
}

/** Fails allocation `attempt`; 1 to go on, 0 after an answer the case does not allow, *ran_whole when none failed. */
static int fail_once(const start_case *each, const clean_run *clean, uint64_t attempt, int *ran_whole) {
    int64_t baseline = spisor_memory_live_blocks();
    spisor_status status;
    int kept;
    printed[0] = '\0';
    fail_allocation(failure_mode, attempt);
    status = run_start(each);
    *ran_whole = !spisor_memory_failed();
    spisor_memory_fail_at(0);
    kept = kept_nothing(each, attempt, baseline);
    if (kept && (status != SPISOR_OUT_OF_MEMORY || *ran_whole) &&
        (status != clean->status || strcmp(printed, clean->printed) != 0)) {
        fprintf(stderr, "%s: failing allocation %llu answered %d \"%s\"; a clean run answers %d \"%s\"\n",
                each->label, (unsigned long long)attempt, (int)status, printed, (int)clean->status, clean->printed);
        kept = 0;
    }
    return kept;
}

static void every_failure_in_mode(const start_case *each) {
    clean_run clean;
    uint64_t attempt = 1;
    int ran_whole = 0;
    printed[0] = '\0';
    spisor_memory_fail_at(0);
    clean.status = run_start(each);
    clean.allocations = spisor_memory_attempts();
    snprintf(clean.printed, sizeof clean.printed, "%s", printed);
    if (clean.status != (spisor_status)each->expected) {
        fprintf(stderr, "%s: a clean run answered %d \"%s\"\n", each->label, (int)clean.status, clean.printed);
        check_failures++;
    }
    for (; check_failures == 0 && !ran_whole; attempt++) {
        if (!fail_once(each, &clean, attempt, &ran_whole)) {
            check_failures++;
        }
    }
    printf("%s%s: %llu allocations, failures 1 to %llu checked\n", each->label,
           allocation_failure_mode_name(failure_mode), (unsigned long long)clean.allocations,
           (unsigned long long)(attempt - 1));
}

/** A run's calls after its start: the outcome its plugin is told, whether this process holds the loader, a tick. */
typedef struct progress_case {
    const char *label;
    const char *const *arguments;
    size_t argument_count;
    uint32_t outcome;
    int held;
    /** What a clean run's stop answers. */
    uint32_t exit_code;
} progress_case;

static void put_status(const char *call, weivel_status status) {
    char text[64];
    snprintf(text, sizeof text, "%s %d", call, (int)status);
    put(text);
}

/** Tells the started run its outcome, then ticks a SERVE run or finishes an ACTION run, `*failed` set on OOM. */
static void tell_progress(const progress_case *each, const weivel_standard_host_plan *plan, int *failed) {
    const weivel_standard_host *host = host_definition();
    weivel_standard_value answered = {WEIVEL_STANDARD_VALUE_NONE, 0, 0.0, NULL, 0, NULL, NULL, 0};
    weivel_standard_host_outcome outcome = {plan->plugins[0].manifest, each->outcome, "its scope was refused"};
    weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
    weivel_standard_service_refusal later = WEIVEL_INIT(weivel_standard_service_refusal);
    weivel_standard_host_next next = WEIVEL_INIT(weivel_standard_host_next);
    weivel_status status = host->started(host->context, &outcome, 1, &refusal);
    put_status("started", status);
    put(refusal.detail);
    *failed = status == WEIVEL_STATUS_OUT_OF_MEMORY;
    host->release_refusal(host->context, &refusal);
    if (status != WEIVEL_STATUS_OK) {
        return;
    }
    status = plan->mode == WEIVEL_STANDARD_HOST_SERVE ? host->tick(host->context, 1700000000000LL, &next, &later)
                                                      : host->finished(host->context, &answered, NULL, &later);
    put_status(plan->mode == WEIVEL_STANDARD_HOST_SERVE ? "tick" : "finished", status);
    put(later.detail);
    *failed = status == WEIVEL_STATUS_OUT_OF_MEMORY;
    if (*failed && (next.has_next != 0 || next.next != 0)) {
        put("an out-of-memory tick answered a next tick");
    }
    host->release_next(host->context, &next);
    host->release_refusal(host->context, &later);
}

/** Removes the newer plugin version a tick's update check installs, so each run's check installs it again. */
static void remove_the_newer_plugin(void) {
    char *folder = joined(cache, "bundles/a/1.1.0");
    CHECK(runtime_remove_tree(folder) == SPISOR_OK);
    spisor_memory_release(folder);
}

static void place_the_holders(int held) {
    char *path = processes_file(".json");
    if (held) {
        hold_the_loader();
    } else {
        CHECK(runtime_remove_tree(path) == SPISOR_OK);
    }
    spisor_memory_release(path);
}

/**
 * Starts the case's run with every allocation allowed, then fails allocation `attempt` of its later calls, 0 for
 * none, and stops it; *failed when a call answered OUT_OF_MEMORY, whose run then answers its failed exit code.
 */
static void run_progress(const progress_case *each, uint64_t attempt, int *failed, uint64_t *allocations) {
    const weivel_standard_host *host = host_definition();
    weivel_standard_host_plan plan;
    uint32_t exit_code = 999;
    char code[32];
    place_the_holders(each->held);
    remove_the_newer_plugin();
    install_plugin("a", PLUGIN_MANIFEST);
    start_empty(&plan, sizeof plan);
    *failed = 0;
    if (host->start(host->context, each->arguments, each->argument_count, &plan, NULL) != WEIVEL_STATUS_OK) {
        put("the start failed");
        return;
    }
    fail_allocation(failure_mode, attempt);
    tell_progress(each, &plan, failed);
    *allocations = spisor_memory_attempts();
    spisor_memory_fail_at(0);
    put(exists_in(cache, "bundles/a/1.1.0/.installed.json") ? "installed" : "not installed");
    host->release_plan(host->context, &plan);
    if (host->stop(host->context, &exit_code, NULL) != WEIVEL_STATUS_OK) {
        put("the stop failed");
    }
    snprintf(code, sizeof code, "exit %u", (unsigned)exit_code);
    put(*failed ? "failed" : code);
    if (*failed && exit_code != RUNTIME_HOST_EXIT_FAILED) {
        put("an out-of-memory run did not fail");
    }
}

static int progress_failure_kept_nothing(const progress_case *each, const char *clean, uint64_t attempt,
                                         int64_t baseline, int failed) {
    if (spisor_memory_live_blocks() != baseline || run_record_count() != 0 || a_run_is_left()) {
        fprintf(stderr, "%s: failing allocation %llu left blocks, a run record or a run\n", each->label,
                (unsigned long long)attempt);
        return 0;
    }
    if (!failed && strcmp(printed, clean) != 0) {
        fprintf(stderr, "%s: failing allocation %llu answered \"%s\"; a clean run answers \"%s\"\n", each->label,
                (unsigned long long)attempt, printed, clean);
        return 0;
    }
    if (failed && strstr(printed, "did not fail") != NULL) {
        fprintf(stderr, "%s: failing allocation %llu: %s\n", each->label, (unsigned long long)attempt, printed);
        return 0;
    }
    return 1;
}

static void every_progress_failure_in_mode(const progress_case *each) {
    char clean[PRINT_CAPACITY];
    char expected[32];
    uint64_t allocations = 0;
    uint64_t attempt = 1;
    uint64_t ignored = 0;
    int failed = 0;
    printed[0] = '\0';
    run_progress(each, 0, &failed, &allocations);
    snprintf(clean, sizeof clean, "%s", printed);
    snprintf(expected, sizeof expected, "exit %u;", (unsigned)each->exit_code);
    if (failed || strstr(clean, expected) == NULL) {
        fprintf(stderr, "%s: a clean run answered \"%s\"\n", each->label, clean);
        check_failures++;
    }
    for (; check_failures == 0 && attempt <= allocations; attempt++) {
        int64_t baseline = spisor_memory_live_blocks();
        printed[0] = '\0';
        run_progress(each, attempt, &failed, &ignored);
        if (!progress_failure_kept_nothing(each, clean, attempt, baseline, failed)) {
            check_failures++;
        }
    }
    printf("%s%s: %llu allocations, failures 1 to %llu checked\n", each->label,
           allocation_failure_mode_name(failure_mode), (unsigned long long)allocations,
           (unsigned long long)(attempt - 1));
}

int main(void) {
    static const char *const serve[1] = {LOADER};
    static const char *const action[5] = {LOADER, "action", "a", "greet", "{\"name\": \"you\"}"};
    static const char *const undeclared[5] = {LOADER, "action", "a", "wave", "{}"};
    static const start_case cases[] = {{"a serve start and its stop", serve, 1, WEIVEL_STATUS_OK},
                                       {"an action start and its stop", action, 5, WEIVEL_STATUS_OK},
                                       {"an undeclared action's start", undeclared, 5, WEIVEL_STATUS_REFUSED}};
    static const progress_case progress_cases[] = {
        {"a held serve run's started and tick", serve, 1, WEIVEL_STANDARD_HOST_ACTIVATED, 1, 0},
        {"a serve run's tick no holder keeps", serve, 1, WEIVEL_STANDARD_HOST_ACTIVATED, 0, 0},
        {"a serve run's refused plugin", serve, 1, WEIVEL_STANDARD_HOST_REFUSED, 0, 0},
        {"an action run's started and finished", action, 5, WEIVEL_STANDARD_HOST_ACTIVATED, 0, 0}};
    spisor_json_use_library_allocator();
    start_host_world("out-of-memory-host");
    list_plugin("a", "1.1.0", "1.1.0");
    write_catalog();
    for (size_t index = 0; index < sizeof cases / sizeof cases[0]; index++) {
        for (failure_mode = FAIL_ONE_ALLOCATION; failure_mode < ALLOCATION_FAILURE_MODES; failure_mode++) {
            every_failure_in_mode(&cases[index]);
        }
    }
    for (size_t index = 0; index < sizeof progress_cases / sizeof progress_cases[0]; index++) {
        for (failure_mode = FAIL_ONE_ALLOCATION; failure_mode < ALLOCATION_FAILURE_MODES; failure_mode++) {
            every_progress_failure_in_mode(&progress_cases[index]);
        }
    }
    finish_base();
    return check_summary("out_of_memory_host_test");
}
