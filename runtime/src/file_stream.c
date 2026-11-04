#include "file_stream.h"

#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>

#include "wide_path.h"

FILE *runtime_open_file(const char *path, runtime_file_mode mode) {
    wchar_t *wide = spisor_wide_path(path);
    if (wide == NULL) {
        return NULL;
    }
    FILE *file = _wfopen(wide, mode == RUNTIME_FILE_READ ? L"rb" : L"wb");
    free(wide);
    return file;
}

int runtime_move_file_over(const char *from, const char *to) {
    wchar_t *wide_from = spisor_wide_path(from);
    wchar_t *wide_to = spisor_wide_path(to);
    int moved = wide_from != NULL && wide_to != NULL &&
                MoveFileExW(wide_from, wide_to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    free(wide_from);
    free(wide_to);
    return moved;
}

#else

FILE *runtime_open_file(const char *path, runtime_file_mode mode) {
    return fopen(path, mode == RUNTIME_FILE_READ ? "rb" : "wb");
}

int runtime_move_file_over(const char *from, const char *to) {
    return rename(from, to) == 0;
}

#endif
