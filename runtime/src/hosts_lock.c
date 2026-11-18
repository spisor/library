#include "hosts_lock.h"
#include "allocation.h"

#include <stdlib.h>

#include "paths.h"
#include "spisor/file.h"
#include "spisor/path.h"

static spisor_status lock_path(const char *state, char **path) {
    const char *directory_segments[2] = {state, "hosts"};
    char *directory = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, directory_segments, 2);
    *path = NULL;
    if (directory == NULL) {
        return SPISOR_OUT_OF_MEMORY;
    }
    if (!spisor_make_directories(directory)) {
        spisor_memory_release(directory);
        return SPISOR_REFUSED;
    }
    const char *segments[2] = {directory, ".lock"};
    *path = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
    spisor_memory_release(directory);
    return *path == NULL ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
}

spisor_status runtime_hosts_lock(const char *state, spisor_lock **guard) {
    char *path;
    spisor_status status = lock_path(state, &path);
    *guard = NULL;
    if (status == SPISOR_OK) {
        status = spisor_lock_file(path, guard);
    }
    spisor_memory_release(path);
    return status;
}
