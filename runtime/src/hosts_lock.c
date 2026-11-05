#include "hosts_lock.h"

#include <stdlib.h>

#include "paths.h"
#include "spisor/file.h"
#include "spisor/path.h"

static char *lock_path(const char *state) {
    const char *directory_segments[2] = {state, "hosts"};
    char *directory = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, directory_segments, 2);
    if (directory == NULL || !spisor_make_directories(directory)) {
        free(directory);
        return NULL;
    }
    const char *segments[2] = {directory, ".lock"};
    char *path = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
    free(directory);
    return path;
}

spisor_lock *runtime_hosts_lock(const char *state) {
    char *path = lock_path(state);
    spisor_lock *guard = path == NULL ? NULL : spisor_lock_file(path);
    free(path);
    return guard;
}
