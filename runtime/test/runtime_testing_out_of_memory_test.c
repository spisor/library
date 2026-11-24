#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <string.h>

#include "allocation_failure.h"
#include "catalog_call.h"
#include "runtime_testing_fixture.h"
#include "spisor/json_allocation.h"

/**
 * @file
 * Lays a home out with each allocation failing in turn: every failure answers OUT_OF_MEMORY with the answer empty and
 * every block freed, and a single failure leaves the base empty, since the emptying it runs can allocate again.
 */

#define MANIFEST                                                                                                     \
    "{\"id\": \"a\", \"version\": \"1.0.0\", \"entry\": \"index.js\", \"settings\": {\"token\": {\"type\": \"secret\"}}}"

static spisor_value_tree manifest;
static spisor_value_tree settings;

static spisor_status lay_out_failing(allocation_failure_mode mode, uint64_t attempt, uint64_t *attempts) {
    runtime_testing_install install = {&manifest.root, entry_source, &settings.root};
    runtime_boot_library libraries[2];
    runtime_testing_home_argument argument;
    runtime_testing_home_answer answer;
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    default_libraries(libraries);
    argument = home_argument(base, &install, 1, libraries);
    start_empty(&answer, sizeof answer);
    fail_allocation(mode, attempt);
    status = testing()->lay_out_home(NULL, &argument, &answer, &refusal);
    *attempts = spisor_memory_attempts();
    spisor_memory_fail_at(0);
    if (status != SPISOR_OK && status != SPISOR_OUT_OF_MEMORY) {
        fprintf(stderr, "failing allocation %llu was refused: %s\n", (unsigned long long)attempt,
                refusal.detail == NULL ? "(none)" : refusal.detail);
    }
    testing()->release_refusal(NULL, &refusal);
    status = empty_unless_answered(status, &answer, sizeof answer);
    testing()->release_home_answer(NULL, &answer);
    return status;
}

static int failure_kept_nothing(allocation_failure_mode mode, uint64_t attempt, spisor_status status,
                                int64_t baseline) {
    if (status != SPISOR_OK && status != SPISOR_OUT_OF_MEMORY) {
        fprintf(stderr, "failing allocation %llu answered %d\n", (unsigned long long)attempt, (int)status);
        return 0;
    }
    if (spisor_memory_live_blocks() != baseline) {
        fprintf(stderr, "failing allocation %llu left blocks\n", (unsigned long long)attempt);
        return 0;
    }
    if (status == SPISOR_OUT_OF_MEMORY && mode == FAIL_ONE_ALLOCATION && !is_empty_folder(base)) {
        fprintf(stderr, "failing allocation %llu left the base holding files\n", (unsigned long long)attempt);
        return 0;
    }
    return 1;
}

static void every_failure_in_mode(allocation_failure_mode mode) {
    uint64_t allocations = 0;
    uint64_t ignored = 0;
    uint64_t attempt = 1;
    start_empty_directory(base);
    CHECK(lay_out_failing(mode, 0, &allocations) == SPISOR_OK);
    for (; check_failures == 0 && attempt <= allocations; attempt++) {
        int64_t baseline;
        start_empty_directory(base);
        baseline = spisor_memory_live_blocks();
        if (!failure_kept_nothing(mode, attempt, lay_out_failing(mode, attempt, &ignored), baseline)) {
            check_failures++;
        }
    }
    printf("lay_out_home%s: %llu allocations, failures 1 to %llu checked\n", allocation_failure_mode_name(mode),
           (unsigned long long)allocations, (unsigned long long)(attempt - 1));
}

int main(void) {
    spisor_json_use_library_allocator();
    start_parent("out-of-memory");
    manifest = tree_of(parsed(MANIFEST));
    settings = tree_of(parsed("{\"token\": \"held\", \"greeting\": \"hi\"}"));
    in_every_failure_mode(every_failure_in_mode);
    spisor_value_release(&settings);
    spisor_value_release(&manifest);
    finish_parent();
    return check_summary("runtime_testing_out_of_memory_test");
}
