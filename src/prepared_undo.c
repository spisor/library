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
spisor_status spisor_prepare_path(const char *path, spisor_prepared_path *prepared) {
    prepared->path = NULL;
    prepared->wide = NULL;
    if (!spisor_is_convertible_path(path)) {
        return SPISOR_REFUSED;
    }
    prepared->path = copy_of(path);
    prepared->wide = prepared->path == NULL ? NULL : spisor_wide_path(path);
    if (prepared->wide == NULL) {
        spisor_release_prepared_path(prepared);
        return SPISOR_OUT_OF_MEMORY;
    }
    return SPISOR_OK;
}

int spisor_remove_prepared_file(const spisor_prepared_path *prepared) {
    if (prepared->wide == NULL || DeleteFileW(prepared->wide)) {
        return 1;
    }
    DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

int spisor_remove_prepared_directory(const spisor_prepared_path *prepared) {
    if (prepared->wide == NULL || RemoveDirectoryW(prepared->wide)) {
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

int spisor_move_prepared_file(const spisor_prepared_path *from, const spisor_prepared_path *to) {
    return from->wide != NULL && to->wide != NULL && spisor_windows_move_wide_over(from->wide, to->wide);
}

int spisor_truncate_prepared_file(const spisor_prepared_path *prepared, uint64_t length) {
    DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE handle = prepared->wide == NULL ? INVALID_HANDLE_VALUE
                                           : CreateFileW(prepared->wide, GENERIC_WRITE, sharing, NULL, OPEN_EXISTING,
                                                         FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        return 0;
    }
    LARGE_INTEGER end;
    end.QuadPart = (LONGLONG)length;
    int truncated = SetFilePointerEx(handle, end, NULL, FILE_BEGIN) && SetEndOfFile(handle);
    return CloseHandle(handle) && truncated;
}

int spisor_link_prepared_file(const spisor_prepared_path *existing, const spisor_prepared_path *linked) {
    return existing->wide != NULL && linked->wide != NULL && CreateHardLinkW(linked->wide, existing->wide, NULL) != 0;
}

int spisor_read_prepared_file(const spisor_prepared_path *prepared, uint8_t *buffer, size_t capacity, size_t *length) {
    *length = 0;
    DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE handle = prepared->wide == NULL ? INVALID_HANDLE_VALUE
                                           : CreateFileW(prepared->wide, GENERIC_READ, sharing, NULL, OPEN_EXISTING,
                                                         FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        return 0;
    }
    int read = 1;
    while (read && *length < capacity) {
        DWORD chunk = 0;
        DWORD wanted = capacity - *length > 0x40000000u ? 0x40000000u : (DWORD)(capacity - *length);
        read = ReadFile(handle, buffer + *length, wanted, &chunk, NULL) != 0;
        if (read && chunk == 0) {
            break;
        }
        *length += chunk;
    }
    CloseHandle(handle);
    return read;
}

void spisor_release_prepared_path(spisor_prepared_path *prepared) {
    spisor_memory_release(prepared->wide);
    spisor_memory_release(prepared->path);
    prepared->wide = NULL;
    prepared->path = NULL;
}
#else
spisor_status spisor_prepare_path(const char *path, spisor_prepared_path *prepared) {
    prepared->path = copy_of(path);
    return prepared->path == NULL ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
}

int spisor_remove_prepared_file(const spisor_prepared_path *prepared) {
    return prepared->path == NULL || remove(prepared->path) == 0 || errno == ENOENT;
}

int spisor_remove_prepared_directory(const spisor_prepared_path *prepared) {
    return prepared->path == NULL || rmdir(prepared->path) == 0 || errno == ENOENT;
}

int spisor_remove_file_in_place(const char *path) {
    return path == NULL || remove(path) == 0 || errno == ENOENT;
}

void spisor_release_prepared_path(spisor_prepared_path *prepared) {
    spisor_memory_release(prepared->path);
    prepared->path = NULL;
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

int spisor_move_prepared_file(const spisor_prepared_path *from, const spisor_prepared_path *to) {
    return from->path != NULL && to->path != NULL && rename(from->path, to->path) == 0;
}

int spisor_truncate_prepared_file(const spisor_prepared_path *prepared, uint64_t length) {
    return prepared->path != NULL && truncate(prepared->path, (off_t)length) == 0;
}

int spisor_link_prepared_file(const spisor_prepared_path *existing, const spisor_prepared_path *linked) {
    return existing->path != NULL && linked->path != NULL && link(existing->path, linked->path) == 0;
}

int spisor_read_prepared_file(const spisor_prepared_path *prepared, uint8_t *buffer, size_t capacity, size_t *length) {
    *length = 0;
    int descriptor = prepared->path == NULL ? -1 : open(prepared->path, O_RDONLY);
    if (descriptor < 0) {
        return 0;
    }
    int read_whole = 1;
    while (*length < capacity) {
        ssize_t chunk = read(descriptor, buffer + *length, capacity - *length);
        if (chunk <= 0) {
            read_whole = chunk == 0;
            break;
        }
        *length += (size_t)chunk;
    }
    close(descriptor);
    return read_whole;
}
#endif

spisor_status spisor_prepare_replacement(const char *path, spisor_prepared_replacement *replacement) {
    memset(replacement, 0, sizeof *replacement);
    char *temporary = spisor_temporary_path_for(path);
    spisor_status prepared = temporary == NULL ? SPISOR_OUT_OF_MEMORY
                                               : spisor_prepare_path(temporary, &replacement->temporary);
    spisor_memory_release(temporary);
    if (prepared == SPISOR_OK) {
        prepared = spisor_prepare_path(path, &replacement->target);
    }
    if (prepared != SPISOR_OK) {
        spisor_release_prepared_replacement(replacement);
    }
    return prepared;
}

int spisor_replace_prepared(const spisor_prepared_replacement *replacement, const uint8_t *bytes, size_t length) {
    int replaced = write_temporary(replacement, bytes, length) &&
                   spisor_move_prepared_file(&replacement->temporary, &replacement->target);
    if (!replaced) {
        spisor_remove_prepared_file(&replacement->temporary);
    }
    return replaced;
}

void spisor_release_prepared_replacement(spisor_prepared_replacement *replacement) {
    spisor_release_prepared_path(&replacement->temporary);
    spisor_release_prepared_path(&replacement->target);
}
