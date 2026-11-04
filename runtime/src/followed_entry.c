#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "followed_entry.h"

#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>

#include "wide_path.h"

/** The attributes of what path leads to, links followed; INVALID_FILE_ATTRIBUTES when nothing answers. */
static DWORD followed_attributes(const char *path) {
    wchar_t *wide = spisor_wide_path(path);
    HANDLE handle = wide == NULL ? INVALID_HANDLE_VALUE
                                 : CreateFileW(wide, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(wide);
    BY_HANDLE_FILE_INFORMATION information;
    DWORD attributes = INVALID_FILE_ATTRIBUTES;
    if (handle != INVALID_HANDLE_VALUE && GetFileInformationByHandle(handle, &information)) {
        attributes = information.dwFileAttributes;
    }
    if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
    }
    return attributes;
}

int runtime_is_followed_directory(const char *path) {
    DWORD attributes = followed_attributes(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

int runtime_is_followed_file(const char *path, int require_execute_bit) {
    (void)require_execute_bit;
    DWORD attributes = followed_attributes(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}
#else
#include <sys/stat.h>

int runtime_is_followed_directory(const char *path) {
    struct stat status;
    return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

int runtime_is_followed_file(const char *path, int require_execute_bit) {
    struct stat status;
    return stat(path, &status) == 0 && S_ISREG(status.st_mode) &&
           (!require_execute_bit || (status.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0);
}
#endif
