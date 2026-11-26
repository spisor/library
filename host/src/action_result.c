#include "action_result.h"
#include "allocation.h"

#include <stdio.h>

#include "directory_tree.h"
#include "paths.h"
#include "run_result.h"
#include "spisor/file.h"
#include "spisor/path.h"

char *host_result_path(const host_context *context, int *removable) {
    static unsigned long taken;
    char name[64];
    snprintf(name, sizeof name, "%.0f-%lu.json", host_process_id(), ++taken);
    const char *segments[3] = {context->state, RUNTIME_RESULTS_FOLDER, name};
    char *path = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 3);
    runtime_entry_outcome cleared = path == NULL ? RUNTIME_ENTRY_OUT_OF_MEMORY : runtime_remove_file_entry(path);
    *removable = cleared == RUNTIME_ENTRY_DONE || cleared == RUNTIME_ENTRY_OUT_OF_MEMORY;
    if (cleared != RUNTIME_ENTRY_DONE) {
        spisor_memory_release(path);
        return NULL;
    }
    return path;
}

char *host_take_result(const char *path, int exit_code, int *out_of_memory) {
    size_t length = 0;
    spisor_read_outcome outcome = SPISOR_READ_FAILED;
    char *text = exit_code == 0 ? (char *)spisor_read_file_reporting(path, &length, &outcome) : NULL;
    *out_of_memory = runtime_remove_file_entry(path) == RUNTIME_ENTRY_OUT_OF_MEMORY ||
                     outcome == SPISOR_READ_OUT_OF_MEMORY;
    return text;
}
