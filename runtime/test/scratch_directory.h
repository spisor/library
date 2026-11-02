#ifndef RUNTIME_TEST_SCRATCH_DIRECTORY_H
#define RUNTIME_TEST_SCRATCH_DIRECTORY_H

#include "check.h"
#include "directory_tree.h"
#include "spisor/file.h"

/**
 * Removes whatever stands at path, never following a link, and makes it again as an empty directory.
 *
 * @implNote A scratch name carries the process id, and Windows reuses process ids, so without this a
 * run inherits the registrations and files an earlier run left under the same name.
 */
static inline void start_empty_directory(const char *path) {
    CHECK(runtime_remove_tree(path) == 1);
    CHECK(spisor_make_directories(path) == 1);
}

#endif
