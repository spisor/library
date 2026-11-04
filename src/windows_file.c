#if defined(_WIN32)
#include "windows_file.h"

#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <wchar.h>
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

/** Whether destination can never be replaced by a rename, however long one waits: a directory or a read-only file. */
static int is_unreplaceable(const wchar_t *destination) {
    DWORD attributes = GetFileAttributesW(destination);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY)) != 0;
}

/** Whether a failed rename is worth trying again: the destination is held by another process for a moment. */
static int is_worth_retrying(DWORD error, const wchar_t *destination) {
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) {
        return 1;
    }
    return error == ERROR_ACCESS_DENIED && !is_unreplaceable(destination);
}

typedef int (*retry_test)(DWORD error, const wchar_t *destination);

static int move_retrying(const char *from, const char *to, DWORD flags, retry_test worth_retrying) {
    wchar_t *wide_from = spisor_wide_path(from);
    wchar_t *wide_to = spisor_wide_path(to);
    int moved = 0;
    for (int attempt = 1; wide_from != NULL && wide_to != NULL && !moved; attempt++) {
        moved = MoveFileExW(wide_from, wide_to, flags) != 0;
        if (moved || attempt == MOVE_ATTEMPTS || !worth_retrying(GetLastError(), wide_to)) {
            break;
        }
        Sleep(MOVE_PAUSE_MILLISECONDS);
    }
    free(wide_from);
    free(wide_to);
    return moved;
}

/** Whether destination names an executable, which Windows refuses to replace while it runs as an image. */
static int is_executable_name(const wchar_t *destination) {
    size_t length = wcslen(destination);
    return length > 4 && _wcsicmp(destination + length - 4, L".exe") == 0;
}

#define MOST_SET_ASIDE 64

/** destination.old for number 0, else destination.old-<number>, freed with free. */
static wchar_t *set_aside_name(const wchar_t *destination, int number) {
    size_t length = wcslen(destination) + 24;
    wchar_t *aside = malloc(length * sizeof *aside);
    if (aside != NULL && number == 0) {
        swprintf(aside, length, L"%ls.old", destination);
    } else if (aside != NULL) {
        swprintf(aside, length, L"%ls.old-%d", destination, number);
    }
    return aside;
}

/** Renames destination to the first set-aside name it can take, one still running from an earlier update kept. */
static int set_aside(const wchar_t *destination, wchar_t **aside) {
    for (int number = 0; number < MOST_SET_ASIDE; number++) {
        *aside = set_aside_name(destination, number);
        if (*aside != NULL && MoveFileExW(destination, *aside, MOVEFILE_REPLACE_EXISTING)) {
            return 1;
        }
        free(*aside);
        *aside = NULL;
    }
    return 0;
}

/**
 * Renames the running image destination aside, which Windows allows where it refuses to replace it, then
 * moves from into its place, putting the image back when that fails; 1 on success, 0 having changed nothing.
 */
static int replace_running_image(const char *from, const char *to) {
    wchar_t *wide_from = spisor_wide_path(from);
    wchar_t *wide_to = spisor_wide_path(to);
    wchar_t *aside = NULL;
    int moved = 0;
    if (wide_from != NULL && wide_to != NULL && set_aside(wide_to, &aside)) {
        moved = MoveFileExW(wide_from, wide_to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
        if (!moved) {
            MoveFileExW(aside, wide_to, 0);
        }
    }
    free(aside);
    free(wide_to);
    free(wide_from);
    return moved;
}

int spisor_windows_remove_set_aside(const char *path) {
    wchar_t *wide = spisor_wide_path(path);
    if (wide == NULL || !is_executable_name(wide)) {
        free(wide);
        return 0;
    }
    int left = 0;
    for (int number = 0; number < MOST_SET_ASIDE; number++) {
        wchar_t *aside = set_aside_name(wide, number);
        DWORD attributes = aside == NULL ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(aside);
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && !DeleteFileW(aside)) {
            left++;
        }
        free(aside);
    }
    free(wide);
    return left;
}

static int is_running_image_refusal(DWORD error, const char *to) {
    wchar_t *wide_to = spisor_wide_path(to);
    int image = wide_to != NULL && error == ERROR_ACCESS_DENIED && is_executable_name(wide_to) &&
                GetFileAttributesW(wide_to) != INVALID_FILE_ATTRIBUTES && !is_unreplaceable(wide_to);
    free(wide_to);
    return image;
}

int spisor_windows_move_over(const char *from, const char *to) {
    wchar_t *wide_from = spisor_wide_path(from);
    wchar_t *wide_to = spisor_wide_path(to);
    int moved = wide_from != NULL && wide_to != NULL &&
                MoveFileExW(wide_from, wide_to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    DWORD error = moved ? ERROR_SUCCESS : GetLastError();
    free(wide_to);
    free(wide_from);
    if (moved) {
        return 1;
    }
    if (is_running_image_refusal(error, to) && replace_running_image(from, to)) {
        return 1;
    }
    return move_retrying(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH, is_worth_retrying);
}

/** Whether a failed directory move is worth trying again: a file inside the folder is held open for a moment. */
static int is_worth_retrying_directory(DWORD error, const wchar_t *destination) {
    (void)destination;
    return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION || error == ERROR_ACCESS_DENIED;
}

int spisor_windows_move_directory(const char *from, const char *to) {
    return move_retrying(from, to, 0, is_worth_retrying_directory);
}
#else
typedef int spisor_windows_file_is_windows_only;
#endif
