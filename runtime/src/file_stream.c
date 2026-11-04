#include "file_stream.h"

#if defined(_WIN32)
#include "windows_file.h"

FILE *runtime_open_file(const char *path, runtime_file_mode mode) {
    return spisor_windows_open(path, mode != RUNTIME_FILE_READ);
}

int runtime_move_file_over(const char *from, const char *to) {
    return spisor_windows_move_over(from, to);
}

#else

FILE *runtime_open_file(const char *path, runtime_file_mode mode) {
    return fopen(path, mode == RUNTIME_FILE_READ ? "rb" : "wb");
}

int runtime_move_file_over(const char *from, const char *to) {
    return rename(from, to) == 0;
}

#endif
