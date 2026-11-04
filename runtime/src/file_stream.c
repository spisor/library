#include "file_stream.h"

#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>

static wchar_t *to_wide(const char *text) {
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (length <= 0) {
        return NULL;
    }
    wchar_t *wide = malloc((size_t)length * sizeof(wchar_t));
    if (wide != NULL && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, length) <= 0) {
        free(wide);
        return NULL;
    }
    return wide;
}

FILE *runtime_open_file(const char *path, runtime_file_mode mode) {
    wchar_t *wide = to_wide(path);
    if (wide == NULL) {
        return NULL;
    }
    FILE *file = _wfopen(wide, mode == RUNTIME_FILE_READ ? L"rb" : L"wb");
    free(wide);
    return file;
}

int runtime_move_file_over(const char *from, const char *to) {
    wchar_t *wide_from = to_wide(from);
    wchar_t *wide_to = to_wide(to);
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
