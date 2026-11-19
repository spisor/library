#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "memory_error.h"
#include "spisor/file.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(memory_sanitizer)
#define SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define SANITIZED 1
#endif

#if defined(__linux__) && !defined(SANITIZED)
#define CAN_EXHAUST_MEMORY 1
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static void only_memory_running_out_counts(void) {
    CHECK(spisor_is_memory_errno(ENOMEM));
    CHECK(!spisor_is_memory_errno(0) && !spisor_is_memory_errno(ENOENT) && !spisor_is_memory_errno(EACCES));
#if defined(_WIN32)
    CHECK(spisor_is_memory_windows_error(ERROR_NOT_ENOUGH_MEMORY) && spisor_is_memory_windows_error(ERROR_OUTOFMEMORY));
    CHECK(!spisor_is_memory_windows_error(ERROR_SUCCESS) && !spisor_is_memory_windows_error(ERROR_FILE_NOT_FOUND) &&
          !spisor_is_memory_windows_error(ERROR_ACCESS_DENIED));
#endif
}

#if defined(CAN_EXHAUST_MEMORY)
/** The bytes this process's address space holds now, from /proc/self/statm; 0 when it cannot be read. */
static unsigned long long address_space_bytes(void) {
    unsigned long long pages = 0;
    FILE *statm = fopen("/proc/self/statm", "r");
    if (statm == NULL) {
        return 0;
    }
    int scanned = fscanf(statm, "%llu", &pages);
    fclose(statm);
    return scanned == 1 ? pages * (unsigned long long)sysconf(_SC_PAGESIZE) : 0;
}

/**
 * Caps the address space a little above what it holds and takes all of the cap, so the C library's own malloc fails.
 *
 * @implNote Runs only in a forked child, which exits without returning what it took.
 */
static int exhaust_memory(void) {
    unsigned long long held = address_space_bytes();
    struct rlimit limit;
    if (held == 0 || getrlimit(RLIMIT_AS, &limit) != 0) {
        return 0;
    }
    limit.rlim_cur = (rlim_t)(held + 4u * 1024u * 1024u);
    if (setrlimit(RLIMIT_AS, &limit) != 0) {
        return 0;
    }
    for (size_t size = 1u << 20; size >= 16; size /= 2) {
        while (malloc(size) != NULL) {
        }
    }
    return 1;
}

/** In a child with no memory left, a read whose fopen fails must answer out of memory, never a failed read. */
static int reads_out_of_memory_in_an_exhausted_child(const char *path) {
    if (!exhaust_memory()) {
        return 2;
    }
    size_t length = 0;
    spisor_read_outcome outcome = SPISOR_READ_FAILED;
    uint8_t *bytes = spisor_read_file_reporting(path, &length, &outcome);
    return bytes == NULL && outcome == SPISOR_READ_OUT_OF_MEMORY ? 0 : 1;
}

static void an_fopen_without_memory_reads_as_out_of_memory(void) {
    char path[1024];
    snprintf(path, sizeof path, "%s/memory-error-%d.txt", SPISOR_SCRATCH_PATH, (int)getpid());
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL && fputs("held", file) >= 0 && fclose(file) == 0);
    fflush(stdout);
    fflush(stderr);
    pid_t child = fork();
    if (child == 0) {
        _exit(reads_out_of_memory_in_an_exhausted_child(path));
    }
    int status = 0;
    CHECK(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status));
    if (WIFEXITED(status) && WEXITSTATUS(status) == 2) {
        printf("memory_error_test: the address space could not be capped, so fopen was not exhausted\n");
    } else {
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    remove(path);
}
#endif

int main(void) {
    only_memory_running_out_counts();
#if defined(CAN_EXHAUST_MEMORY)
    an_fopen_without_memory_reads_as_out_of_memory();
#endif
    return check_summary("memory_error_test");
}
