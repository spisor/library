#if defined(_WIN32)
#include "windows_file.h"

#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <windows.h>

#include "wide_path.h"

#define MOVE_ATTEMPTS 40
#define MOVE_PAUSE_MILLISECONDS 25

static FILE *stream_of(HANDLE handle, int writing) {
    int descriptor = _open_osfhandle((intptr_t)handle, (writing ? _O_WRONLY : _O_RDONLY) | _O_BINARY);
    if (descriptor < 0) {
        CloseHandle(handle);
        return NULL;
    }
    FILE *file = _fdopen(descriptor, writing ? "wb" : "rb");
    if (file == NULL) {
        _close(descriptor);
    }
    return file;
}

FILE *spisor_windows_open(const char *path, int writing) {
    wchar_t *wide = spisor_wide_path(path);
    if (wide == NULL) {
        return NULL;
    }
    HANDLE handle = CreateFileW(wide, writing ? GENERIC_WRITE : GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                writing ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(wide);
    return handle == INVALID_HANDLE_VALUE ? NULL : stream_of(handle, writing);
}

static int is_held_elsewhere(DWORD error) {
    return error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED || error == ERROR_LOCK_VIOLATION;
}

int spisor_windows_move_over(const char *from, const char *to) {
    wchar_t *wide_from = spisor_wide_path(from);
    wchar_t *wide_to = spisor_wide_path(to);
    int moved = 0;
    for (int attempt = 0; wide_from != NULL && wide_to != NULL && !moved && attempt < MOVE_ATTEMPTS; attempt++) {
        moved = MoveFileExW(wide_from, wide_to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
        if (!moved && !is_held_elsewhere(GetLastError())) {
            break;
        }
        if (!moved) {
            Sleep(MOVE_PAUSE_MILLISECONDS);
        }
    }
    free(wide_from);
    free(wide_to);
    return moved;
}
#else
typedef int spisor_windows_file_is_windows_only;
#endif
