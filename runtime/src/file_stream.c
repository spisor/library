#include "file_stream.h"

#if defined(_WIN32)
#include "windows_file.h"

FILE *runtime_open_file(const char *path, runtime_file_mode mode, int *out_of_memory) {
    return spisor_windows_open(path, mode != RUNTIME_FILE_READ, out_of_memory);
}

spisor_status runtime_move_file_over(const char *from, const char *to) {
    int out_of_memory;
    if (spisor_windows_move_over(from, to, &out_of_memory)) {
        return SPISOR_OK;
    }
    return out_of_memory ? SPISOR_OUT_OF_MEMORY : SPISOR_REFUSED;
}

#else
#include "memory_error.h"

FILE *runtime_open_file(const char *path, runtime_file_mode mode, int *out_of_memory) {
    FILE *file = fopen(path, mode == RUNTIME_FILE_READ ? "rb" : "wb");
    *out_of_memory = file == NULL && spisor_is_memory_errno(errno);
    return file;
}

spisor_status runtime_move_file_over(const char *from, const char *to) {
    return rename(from, to) == 0 ? SPISOR_OK : SPISOR_REFUSED;
}

#endif
