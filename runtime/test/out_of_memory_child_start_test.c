#include "allocation_failure.h"
#include "child_start_fixture.h"
#include "spisor/json_allocation.h"

/**
 * @file
 * Fails processes.run_action's allocations one at a time, and from one on, until a run allocates without failing:
 * every run answers what a clean run answers or OUT_OF_MEMORY with its answer empty, leaves no block live and no file
 * in the results folder, including the runs whose child ran and wrote its result before memory ran out. The one file
 * allowed to stay is one memory that stays out left no room to remove, on an OUT_OF_MEMORY answer, which the cache
 * sweep removes a day later.
 */

/** What one run answered, printed into fixed storage so printing never moves the allocation count. */
typedef struct printed_run {
    spisor_status status;
    char printed[1024];
} printed_run;

static printed_run run_once(const char *action) {
    printed_run printed;
    runtime_processes_run_action_argument argument = action_argument(action);
    runtime_processes_run_action_answer answer;
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    start_empty(&answer, sizeof answer);
    printed.status = processes()->run_action(NULL, &argument, &answer, &refusal);
    snprintf(printed.printed, sizeof printed.printed, "%d %s %s %s", (int)answer.exit_code,
             answer.result != NULL && answer.result->kind == SPISOR_VALUE_TEXT ? answer.result->text : "-",
             answer.failure == NULL ? "-" : answer.failure, refusal.detail == NULL ? "-" : refusal.detail);
    printed.status = empty_unless_answered(printed.status, &answer, sizeof answer);
    processes()->release_run_action_answer(NULL, &answer);
    processes()->release_refusal(NULL, &refusal);
    return printed;
}

/** Whether a result file stands, a sticky failure's leftover on an OUT_OF_MEMORY answer removed and not counted. */
static int leaves_a_result(allocation_failure_mode mode, spisor_status status) {
    int left = results_left() != 0;
    if (left && mode == FAIL_EVERY_LATER_ALLOCATION && status == SPISOR_OUT_OF_MEMORY) {
        char *results = joined(state, "results");
        CHECK(runtime_remove_tree(results) == SPISOR_OK);
        spisor_memory_release(results);
        left = 0;
    }
    return left;
}

static int matches_clean(const printed_run *clean, const printed_run *failed, const char *action, uint64_t attempt) {
    if (failed->status == SPISOR_OUT_OF_MEMORY ||
        (failed->status == clean->status && strcmp(failed->printed, clean->printed) == 0)) {
        return 1;
    }
    fprintf(stderr, "%s: failing allocation %llu answered %d \"%s\"; a clean run answers %d \"%s\"\n", action,
            (unsigned long long)attempt, (int)failed->status, failed->printed, (int)clean->status, clean->printed);
    return 0;
}

static void every_failure_of(const char *action, allocation_failure_mode mode) {
    spisor_memory_fail_at(0);
    printed_run clean = run_once(action);
    uint64_t allocations = spisor_memory_attempts();
    uint64_t attempt = 1;
    int ran_whole = 0;
    CHECK(clean.status == SPISOR_OK && results_left() == 0);
    for (; check_failures == 0 && !ran_whole; attempt++) {
        int64_t baseline = spisor_memory_live_blocks();
        fail_allocation(mode, attempt);
        printed_run failed = run_once(action);
        ran_whole = !spisor_memory_failed();
        spisor_memory_fail_at(0);
        if (spisor_memory_live_blocks() != baseline) {
            fprintf(stderr, "%s: failing allocation %llu left %lld blocks live\n", action, (unsigned long long)attempt,
                    (long long)(spisor_memory_live_blocks() - baseline));
            check_failures++;
        } else if (leaves_a_result(mode, failed.status)) {
            fprintf(stderr, "%s: failing allocation %llu left a result file\n", action, (unsigned long long)attempt);
            check_failures++;
        } else if (!matches_clean(&clean, &failed, action, attempt)) {
            check_failures++;
        }
    }
    printf("run_action %s%s: %llu allocations, failures 1 to %llu checked\n", action,
           allocation_failure_mode_name(mode), (unsigned long long)allocations, (unsigned long long)(attempt - 1));
}

int main(void) {
    spisor_json_use_library_allocator();
    start_child_world("out-of-memory");
    for (int mode = FAIL_ONE_ALLOCATION; mode < ALLOCATION_FAILURE_MODES; mode++) {
        every_failure_of("greet", (allocation_failure_mode)mode);
        every_failure_of("fail", (allocation_failure_mode)mode);
        every_failure_of("noisy", (allocation_failure_mode)mode);
    }
    finish_base();
    return check_summary("out_of_memory_child_start_test");
}
