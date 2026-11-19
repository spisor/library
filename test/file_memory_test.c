#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "allocation.h"
#include "allocation_failure.h"
#include "check.h"
#include "spisor/file.h"

#if defined(_WIN32)
#include <process.h>
#define process_number() _getpid()
#else
#include <unistd.h>
#define process_number() getpid()
#endif

/** A base no earlier run made, so each failure's folder is new and its absence afterwards means nothing was made. */
static char base[400];

static void base_named(char *path, size_t size, const char *kind, allocation_failure_mode mode, uint64_t attempt) {
    snprintf(path, size, "%s/%s-%d-%llu", base, kind, (int)mode, (unsigned long long)attempt);
}

static void folders_are_made_or_memory_is_answered_with_none_made(allocation_failure_mode mode) {
    for (uint64_t attempt = 1; attempt < 100; attempt++) {
        char root[512];
        char nested[600];
        base_named(root, sizeof root, "folders", mode, attempt);
        snprintf(nested, sizeof nested, "%s/one/two", root);
        fail_allocation(mode, attempt);
        spisor_status made = spisor_make_directories(nested);
        int failed = spisor_memory_failed();
        spisor_memory_fail_at(0);
        if (!failed) {
            CHECK(made == SPISOR_OK && spisor_probe_file(nested) == SPISOR_FILE_PRESENT);
            CHECK(spisor_memory_live_blocks() == 0);
            return;
        }
        CHECK(made == SPISOR_OUT_OF_MEMORY);
        CHECK(spisor_probe_file(root) == SPISOR_FILE_MISSING);
        CHECK(spisor_memory_live_blocks() == 0);
    }
    CHECK(0);
}

static void an_append_writes_or_memory_is_answered_with_nothing_made(allocation_failure_mode mode) {
    for (uint64_t attempt = 1; attempt < 100; attempt++) {
        char root[512];
        char file[600];
        base_named(root, sizeof root, "append", mode, attempt);
        snprintf(file, sizeof file, "%s/logs/day.log", root);
        fail_allocation(mode, attempt);
        spisor_status appended = spisor_append_file(file, (const uint8_t *)"line\n", 5);
        int failed = spisor_memory_failed();
        spisor_memory_fail_at(0);
        if (!failed) {
            size_t length = 0;
            uint8_t *bytes = spisor_read_file_reporting(file, &length, &(spisor_read_outcome){SPISOR_READ_FAILED});
            CHECK(appended == SPISOR_OK && bytes != NULL && length == 5 && memcmp(bytes, "line\n", 5) == 0);
            spisor_memory_release(bytes);
            CHECK(spisor_memory_live_blocks() == 0);
            return;
        }
        CHECK(appended == SPISOR_OUT_OF_MEMORY);
        CHECK(spisor_probe_file(root) == SPISOR_FILE_MISSING);
        CHECK(spisor_memory_live_blocks() == 0);
    }
    CHECK(0);
}

static void a_link_question_answers_or_reports_memory(allocation_failure_mode mode) {
    for (uint64_t attempt = 1; attempt < 100; attempt++) {
        int out_of_memory = 0;
        fail_allocation(mode, attempt);
        int linked = spisor_is_link(base, &out_of_memory);
        int failed = spisor_memory_failed();
        spisor_memory_fail_at(0);
        CHECK(linked == 0 && out_of_memory == failed);
        if (!failed) {
            return;
        }
    }
    CHECK(0);
}

int main(void) {
    spisor_memory_fail_at(0);
    snprintf(base, sizeof base, SPISOR_SCRATCH_PATH "/file-memory-%d", (int)process_number());
    CHECK(spisor_make_directories(base) == SPISOR_OK);
    in_every_failure_mode(folders_are_made_or_memory_is_answered_with_none_made);
    in_every_failure_mode(an_append_writes_or_memory_is_answered_with_nothing_made);
    in_every_failure_mode(a_link_question_answers_or_reports_memory);
    return check_summary("file_memory_test");
}
