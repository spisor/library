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
 * and no run left to stop, or to answer as a clean run does.
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
    weivel_status status = host->tick(host->context, 0, &next, NULL);
    host->release_next(host->context, &next);
    return status != WEIVEL_STATUS_REFUSED;
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

int main(void) {
    static const char *const serve[1] = {LOADER};
    static const char *const action[5] = {LOADER, "action", "a", "greet", "{\"name\": \"you\"}"};
    static const char *const undeclared[5] = {LOADER, "action", "a", "wave", "{}"};
    static const start_case cases[] = {{"a serve start and its stop", serve, 1, WEIVEL_STATUS_OK},
                                       {"an action start and its stop", action, 5, WEIVEL_STATUS_OK},
                                       {"an undeclared action's start", undeclared, 5, WEIVEL_STATUS_REFUSED}};
    spisor_json_use_library_allocator();
    start_host_world("out-of-memory-host");
    for (size_t index = 0; index < sizeof cases / sizeof cases[0]; index++) {
        for (failure_mode = FAIL_ONE_ALLOCATION; failure_mode < ALLOCATION_FAILURE_MODES; failure_mode++) {
            every_failure_in_mode(&cases[index]);
        }
    }
    finish_base();
    return check_summary("out_of_memory_host_test");
}
