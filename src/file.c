#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#endif

#include "spisor/file.h"

#include "file_presence.h"
#include "file_replacement.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static char *duplicate_text(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy != NULL) {
        memcpy(copy, text, size);
    }
    return copy;
}

#if defined(_WIN32)
static wchar_t *to_wide(const char *path) {
    int wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (wide_length <= 0) {
        return NULL;
    }
    wchar_t *wide = malloc((size_t)wide_length * sizeof(wchar_t));
    if (wide != NULL && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, wide_length) <= 0) {
        free(wide);
        return NULL;
    }
    return wide;
}

/** GetFileAttributesW rather than _wstat, because _wstat refuses a directory named with a trailing separator. */
int spisor_file_exists(const char *path) {
    wchar_t *wide = to_wide(path);
    if (wide == NULL) {
        return 0;
    }
    DWORD attributes = GetFileAttributesW(wide);
    free(wide);
    return attributes != INVALID_FILE_ATTRIBUTES;
}

spisor_file_presence spisor_probe_file(const char *path) {
    wchar_t *wide = to_wide(path);
    if (wide == NULL) {
        return SPISOR_FILE_UNKNOWN;
    }
    DWORD attributes = GetFileAttributesW(wide);
    DWORD error = GetLastError();
    free(wide);
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        return SPISOR_FILE_PRESENT;
    }
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? SPISOR_FILE_MISSING : SPISOR_FILE_UNKNOWN;
}

static FILE *open_for_reading(const char *path) {
    wchar_t *wide = to_wide(path);
    if (wide == NULL) {
        return NULL;
    }
    FILE *file = _wfopen(wide, L"rb");
    free(wide);
    return file;
}

static int is_directory(const char *path) {
    wchar_t *wide = to_wide(path);
    if (wide == NULL) {
        return 0;
    }
    DWORD attributes = GetFileAttributesW(wide);
    free(wide);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}
static FILE *open_for_writing(const char *path, const wchar_t *mode) {
    wchar_t *wide = to_wide(path);
    if (wide == NULL) {
        return NULL;
    }
    FILE *file = _wfopen(wide, mode);
    free(wide);
    return file;
}

static FILE *open_for_appending(const char *path) {
    return open_for_writing(path, L"ab");
}

static FILE *create_temporary(const char *temporary, const char *target, int owner_only) {
    (void)target;
    (void)owner_only;
    return open_for_writing(temporary, L"wb");
}

static int create_directory(const char *path) {
    wchar_t *wide = to_wide(path);
    if (wide == NULL) {
        return 0;
    }
    int created = CreateDirectoryW(wide, NULL) != 0;
    free(wide);
    return created;
}

static void remove_file(const char *path) {
    wchar_t *wide = to_wide(path);
    if (wide != NULL) {
        DeleteFileW(wide);
        free(wide);
    }
}

static int move_over(const char *from, const char *to) {
    wchar_t *wide_from = to_wide(from);
    wchar_t *wide_to = to_wide(to);
    int moved = wide_from != NULL && wide_to != NULL
        && MoveFileExW(wide_from, wide_to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    free(wide_from);
    free(wide_to);
    return moved;
}

static char *to_utf8(const wchar_t *wide) {
    int length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (length <= 0) {
        return NULL;
    }
    char *narrow = malloc((size_t)length);
    if (narrow != NULL && WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow, length, NULL, NULL) <= 0) {
        free(narrow);
        return NULL;
    }
    return narrow;
}

/** A drive-letter path loses its extended-length prefix; a UNC path keeps it, which the wide APIs accept. */
static const wchar_t *without_extended_prefix(const wchar_t *final_path) {
    int has_drive_prefix = wcsncmp(final_path, L"\\\\?\\", 4) == 0 && final_path[4] != 0 && final_path[5] == L':';
    return has_drive_prefix ? final_path + 4 : final_path;
}

static char *final_path_of_link(const wchar_t *wide) {
    HANDLE handle = CreateFileW(wide, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    DWORD needed = GetFinalPathNameByHandleW(handle, NULL, 0, FILE_NAME_NORMALIZED);
    wchar_t *final_path = needed == 0 ? NULL : malloc((size_t)needed * sizeof(wchar_t));
    char *resolved = NULL;
    if (final_path != NULL && GetFinalPathNameByHandleW(handle, final_path, needed, FILE_NAME_NORMALIZED) < needed) {
        resolved = to_utf8(without_extended_prefix(final_path));
    }
    free(final_path);
    CloseHandle(handle);
    return resolved;
}

int spisor_is_link(const char *path) {
    wchar_t *wide = to_wide(path);
    if (wide == NULL) {
        return 0;
    }
    DWORD attributes = GetFileAttributesW(wide);
    free(wide);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

static char *write_target_of(const char *path) {
    if (!spisor_is_link(path)) {
        return duplicate_text(path);
    }
    wchar_t *wide = to_wide(path);
    char *target = wide == NULL ? NULL : final_path_of_link(wide);
    free(wide);
    return target;
}
#else
int spisor_file_exists(const char *path) {
    struct stat status;
    return path[0] != '\0' && stat(path, &status) == 0;
}

spisor_file_presence spisor_probe_file(const char *path) {
    struct stat status;
    if (stat(path, &status) == 0) {
        return SPISOR_FILE_PRESENT;
    }
    return errno == ENOENT || errno == ENOTDIR ? SPISOR_FILE_MISSING : SPISOR_FILE_UNKNOWN;
}

static FILE *open_for_reading(const char *path) {
    return fopen(path, "rb");
}

static int is_directory(const char *path) {
    struct stat status;
    return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

static FILE *open_for_appending(const char *path) {
    return fopen(path, "ab");
}

static mode_t replacement_mode(const char *target, int owner_only, int *exact) {
    struct stat status;
    if (owner_only) {
        *exact = 1;
        return 0600;
    }
    *exact = stat(target, &status) == 0;
    return *exact ? status.st_mode & 07777 : 0666;
}

static FILE *create_temporary(const char *temporary, const char *target, int owner_only) {
    int exact;
    mode_t mode = replacement_mode(target, owner_only, &exact);
    int descriptor = open(temporary, O_CREAT | O_EXCL | O_WRONLY, mode);
    if (descriptor < 0) {
        return NULL;
    }
    FILE *file = (!exact || fchmod(descriptor, mode) == 0) ? fdopen(descriptor, "wb") : NULL;
    if (file == NULL) {
        close(descriptor);
        remove(temporary);
    }
    return file;
}

static int create_directory(const char *path) {
    return mkdir(path, 0777) == 0;
}

static void remove_file(const char *path) {
    remove(path);
}

static int move_over(const char *from, const char *to) {
    return rename(from, to) == 0;
}

int spisor_is_link(const char *path) {
    struct stat status;
    return lstat(path, &status) == 0 && S_ISLNK(status.st_mode);
}

static char *write_target_of(const char *path) {
    return spisor_is_link(path) ? realpath(path, NULL) : duplicate_text(path);
}
#endif

static uint8_t *read_stream(FILE *file, size_t *length) {
    size_t capacity = 4096;
    size_t used = 0;
    uint8_t *bytes = malloc(capacity);
    while (bytes != NULL) {
        if (used + 1 >= capacity) {
            uint8_t *grown = realloc(bytes, capacity * 2);
            if (grown == NULL) {
                break;
            }
            bytes = grown;
            capacity *= 2;
        }
        size_t read = fread(bytes + used, 1, capacity - used - 1, file);
        used += read;
        if (read == 0) {
            if (ferror(file)) {
                break;
            }
            bytes[used] = '\0';
            *length = used;
            return bytes;
        }
    }
    free(bytes);
    return NULL;
}

uint8_t *spisor_read_file(const char *path, size_t *length) {
    if (path[0] == '\0' || is_directory(path)) {
        return NULL;
    }
    FILE *file = open_for_reading(path);
    if (file == NULL) {
        return NULL;
    }
    uint8_t *bytes = read_stream(file, length);
    fclose(file);
    return bytes;
}

static int write_and_close(FILE *file, const uint8_t *bytes, size_t length) {
    int written = fwrite(bytes, 1, length, file) == length;
    return fclose(file) == 0 && written;
}

static int is_separator(char character) {
    return character == '/' || character == '\\';
}

int spisor_make_directories(const char *path) {
    if (is_directory(path)) {
        return 1;
    }
    size_t path_length = strlen(path);
    char *prefix = malloc(path_length + 1);
    if (prefix == NULL) {
        return 0;
    }
    memcpy(prefix, path, path_length + 1);
    for (size_t index = 1; index < path_length; index++) {
        if (is_separator(prefix[index])) {
            char separator = prefix[index];
            prefix[index] = '\0';
            create_directory(prefix);
            prefix[index] = separator;
        }
    }
    free(prefix);
    create_directory(path);
    return is_directory(path);
}

int spisor_append_file(const char *path, const uint8_t *bytes, size_t length) {
    FILE *file = open_for_appending(path);
    return file != NULL && write_and_close(file, bytes, length);
}

static void seed_once(void) {
    static int seeded = 0;
    if (!seeded) {
        int local = 0;
        srand((unsigned)time(NULL) ^ (unsigned)(size_t)&local);
        seeded = 1;
    }
}

static char *temporary_path_for(const char *path) {
    seed_once();
    size_t path_length = strlen(path);
    char *temporary = malloc(path_length + 1 + 16 + 4 + 1);
    if (temporary == NULL) {
        return NULL;
    }
    do {
        snprintf(temporary, path_length + 1 + 16 + 4 + 1, "%s.%04x%04x%04x%04x.tmp", path,
                 (unsigned)rand() & 0xffffu, (unsigned)rand() & 0xffffu, (unsigned)rand() & 0xffffu,
                 (unsigned)rand() & 0xffffu);
    } while (spisor_file_exists(temporary));
    return temporary;
}

FILE *spisor_open_replacement(const char *target, int owner_only, char **temporary_path) {
    char *temporary = temporary_path_for(target);
    FILE *file = temporary == NULL ? NULL : create_temporary(temporary, target, owner_only);
    if (file == NULL) {
        free(temporary);
        temporary = NULL;
    }
    *temporary_path = temporary;
    return file;
}

static int replace_resolved_file(const char *target, const uint8_t *bytes, size_t length, int owner_only) {
    char *temporary;
    FILE *file = spisor_open_replacement(target, owner_only, &temporary);
    if (file == NULL) {
        return 0;
    }
    int replaced = write_and_close(file, bytes, length) && move_over(temporary, target);
    if (!replaced) {
        remove_file(temporary);
    }
    free(temporary);
    return replaced;
}

static int replace_through_links(const char *path, const uint8_t *bytes, size_t length) {
    char *target = write_target_of(path);
    if (target == NULL) {
        return 0;
    }
    int replaced = replace_resolved_file(target, bytes, length, 0);
    free(target);
    return replaced;
}

int spisor_replace_file(const char *path, const uint8_t *bytes, size_t length) {
    return replace_through_links(path, bytes, length);
}

int spisor_replace_private_file(const char *path, const uint8_t *bytes, size_t length) {
    return !spisor_is_link(path) && replace_resolved_file(path, bytes, length, 1);
}
