#ifndef RUNTIME_TEST_SERVICE_SCRATCH_H
#define RUNTIME_TEST_SERVICE_SCRATCH_H

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#define SCRATCH_PROCESS_ID ((int)getpid())
#else
#include <process.h>
#define SCRATCH_PROCESS_ID ((int)_getpid())
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "paths.h"
#include "scratch_directory.h"
#include "spisor/file.h"
#include "spisor/path.h"

/** A fresh empty directory under the test's scratch place, named by suite, process and label. */
static inline void scratch_in(char *directory, size_t size, const char *suite, const char *label) {
    static int sequence = 0;
    snprintf(directory, size, RUNTIME_SCRATCH_PATH "/%s-%d-%d-%s", suite, SCRATCH_PROCESS_ID, sequence++, label);
    start_empty_directory(directory);
}

/** directory/name joined as this OS's path.join joins it, freed with free. */
static inline char *joined_path(const char *directory, const char *name) {
    const char *segments[2] = {directory, name};
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
}

/** Writes text to directory/name with mode on POSIX (ignored on Windows), answering the joined path. */
static inline char *touch_file(const char *directory, const char *name, const char *text, int mode) {
    char *path = joined_path(directory, name);
    CHECK(path != NULL && spisor_replace_file(path, (const uint8_t *)text, strlen(text)) == 1);
#if !defined(_WIN32)
    CHECK(path != NULL && chmod(path, (mode_t)mode) == 0);
#else
    (void)mode;
#endif
    return path;
}

static inline void make_directory_in(const char *directory, const char *name) {
    char *path = joined_path(directory, name);
    CHECK(path != NULL && spisor_make_directories(path) == 1);
    free(path);
}

#endif
