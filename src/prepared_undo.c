#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "prepared_undo.h"

#include <stdio.h>
#include <string.h>

#include "allocation.h"
#include "file_replacement.h"

#if defined(_WIN32)
#include <windows.h>

#include "wide_path.h"
#include "windows_file.h"

#define LONGEST_WIDE_PATH 32776
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static char *copy_of(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = spisor_memory_allocate(size);
    if (copy != NULL) {
        memcpy(copy, text, size);
    }
    return copy;
}

#if defined(_WIN32)
spisor_status spisor_prepare_removal(const char *path, spisor_prepared_removal *removal) {
    removal->path = NULL;
    removal->wide = NULL;
    if (!spisor_is_convertible_path(path)) {
        return SPISOR_REFUSED;
    }
    removal->path = copy_of(path);
    removal->wide = removal->path == NULL ? NULL : spisor_wide_path(path);
    if (removal->wide == NULL) {
        spisor_release_prepared_removal(removal);
        return SPISOR_OUT_OF_MEMORY;
    }
    return SPISOR_OK;
}

int spisor_remove_prepared(const spisor_prepared_removal *removal) {
    if (removal->wide == NULL || DeleteFileW(removal->wide)) {
        return 1;
    }
    DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

int spisor_remove_prepared_directory(const spisor_prepared_removal *removal) {
    if (removal->wide == NULL || RemoveDirectoryW(removal->wide)) {
        return 1;
    }
    DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

int spisor_remove_file_in_place(const char *path) {
    wchar_t wide[LONGEST_WIDE_PATH];
    if (path == NULL) {
        return 1;
    }
    if (!spisor_wide_path_into(path, wide, LONGEST_WIDE_PATH)) {
        return 0;
    }
    if (DeleteFileW(wide)) {
        return 1;
    }
    DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

static int write_handle(HANDLE handle, const uint8_t *bytes, size_t length) {
    while (length > 0) {
        DWORD chunk = length > 0x40000000u ? 0x40000000u : (DWORD)length;
        DWORD written = 0;
        if (!WriteFile(handle, bytes, chunk, &written, NULL) || written == 0) {
            return 0;
        }
        bytes += written;
        length -= written;
    }
    return 1;
}

static int write_temporary(const spisor_prepared_replacement *replacement, const uint8_t *bytes, size_t length) {
    DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE handle = CreateFileW(replacement->temporary.wide, GENERIC_WRITE, sharing, NULL, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        return 0;
    }
    int written = write_handle(handle, bytes, length);
    return CloseHandle(handle) && written;
}

static int move_temporary(const spisor_prepared_replacement *replacement) {
    return spisor_windows_move_wide_over(replacement->temporary.wide, replacement->target.wide);
}

void spisor_release_prepared_removal(spisor_prepared_removal *removal) {
    spisor_memory_release(removal->wide);
    spisor_memory_release(removal->path);
    removal->wide = NULL;
    removal->path = NULL;
}
#else
spisor_status spisor_prepare_removal(const char *path, spisor_prepared_removal *removal) {
    removal->path = copy_of(path);
    return removal->path == NULL ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
}

int spisor_remove_prepared(const spisor_prepared_removal *removal) {
    return removal->path == NULL || remove(removal->path) == 0 || errno == ENOENT;
}

int spisor_remove_prepared_directory(const spisor_prepared_removal *removal) {
    return removal->path == NULL || rmdir(removal->path) == 0 || errno == ENOENT;
}

int spisor_remove_file_in_place(const char *path) {
    return path == NULL || remove(path) == 0 || errno == ENOENT;
}

void spisor_release_prepared_removal(spisor_prepared_removal *removal) {
    spisor_memory_release(removal->path);
    removal->path = NULL;
}

static mode_t replacement_mode(const char *target) {
    struct stat status;
    return stat(target, &status) == 0 ? status.st_mode & 07777 : 0666;
}

static int write_descriptor(int descriptor, const uint8_t *bytes, size_t length) {
    while (length > 0) {
        ssize_t written = write(descriptor, bytes, length);
        if (written <= 0) {
            return 0;
        }
        bytes += written;
        length -= (size_t)written;
    }
    return 1;
}

static int write_temporary(const spisor_prepared_replacement *replacement, const uint8_t *bytes, size_t length) {
    int descriptor = open(replacement->temporary.path, O_CREAT | O_EXCL | O_WRONLY,
                          replacement_mode(replacement->target.path));
    if (descriptor < 0) {
        return 0;
    }
    int written = write_descriptor(descriptor, bytes, length);
    return close(descriptor) == 0 && written;
}

static int move_temporary(const spisor_prepared_replacement *replacement) {
    return rename(replacement->temporary.path, replacement->target.path) == 0;
}
#endif

spisor_status spisor_prepare_replacement(const char *path, spisor_prepared_replacement *replacement) {
    memset(replacement, 0, sizeof *replacement);
    char *temporary = spisor_temporary_path_for(path);
    spisor_status prepared = temporary == NULL ? SPISOR_OUT_OF_MEMORY
                                               : spisor_prepare_removal(temporary, &replacement->temporary);
    spisor_memory_release(temporary);
    if (prepared == SPISOR_OK) {
        prepared = spisor_prepare_removal(path, &replacement->target);
    }
    if (prepared != SPISOR_OK) {
        spisor_release_prepared_replacement(replacement);
    }
    return prepared;
}

int spisor_replace_prepared(const spisor_prepared_replacement *replacement, const uint8_t *bytes, size_t length) {
    int replaced = write_temporary(replacement, bytes, length) && move_temporary(replacement);
    if (!replaced) {
        spisor_remove_prepared(&replacement->temporary);
    }
    return replaced;
}

void spisor_release_prepared_replacement(spisor_prepared_replacement *replacement) {
    spisor_release_prepared_removal(&replacement->temporary);
    spisor_release_prepared_removal(&replacement->target);
}
