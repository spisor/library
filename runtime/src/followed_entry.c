#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "followed_entry.h"
#include "allocation.h"

#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>

#include "wide_path.h"

/**
 * The attributes of what path leads to, links followed; INVALID_FILE_ATTRIBUTES when nothing answers, with
 * *out_of_memory set to 1 when memory ran out asking.
 */
static DWORD followed_attributes(const char *path, int *out_of_memory) {
    int convertible = spisor_is_convertible_path(path);
    wchar_t *wide = convertible ? spisor_wide_path(path) : NULL;
    *out_of_memory = convertible && wide == NULL;
    HANDLE handle = wide == NULL ? INVALID_HANDLE_VALUE
                                 : CreateFileW(wide, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    spisor_memory_release(wide);
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

int runtime_is_followed_directory(const char *path, int *out_of_memory) {
    DWORD attributes = followed_attributes(path, out_of_memory);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

int runtime_is_followed_file(const char *path, int require_execute_bit, int *out_of_memory) {
    (void)require_execute_bit;
    DWORD attributes = followed_attributes(path, out_of_memory);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}
#else
#include <sys/stat.h>

int runtime_is_followed_directory(const char *path, int *out_of_memory) {
    struct stat status;
    *out_of_memory = 0;
    return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

int runtime_is_followed_file(const char *path, int require_execute_bit, int *out_of_memory) {
    struct stat status;
    *out_of_memory = 0;
    return stat(path, &status) == 0 && S_ISREG(status.st_mode) &&
           (!require_execute_bit || (status.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0);
}
#endif
