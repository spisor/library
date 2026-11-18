#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#endif

#include "spisor/file.h"

#include "allocation.h"
#include "file_reading.h"
#include "file_replacement.h"
#include "prepared_undo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>

#include "wide_path.h"
#include "windows_file.h"
#else
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static char *duplicate_text(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = spisor_memory_allocate(size);
    if (copy != NULL) {
        memcpy(copy, text, size);
    }
    return copy;
}

#if defined(_WIN32)
/** GetFileAttributesW rather than _wstat, because _wstat refuses a directory named with a trailing separator. */
spisor_file_presence spisor_probe_file(const char *path) {
    if (!spisor_is_convertible_path(path)) {
        return SPISOR_FILE_UNKNOWN;
    }
    wchar_t *wide = spisor_wide_path(path);
    if (wide == NULL) {
        return SPISOR_FILE_OUT_OF_MEMORY;
    }
    DWORD attributes = GetFileAttributesW(wide);
    DWORD error = GetLastError();
    spisor_memory_release(wide);
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        return SPISOR_FILE_PRESENT;
    }
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? SPISOR_FILE_MISSING : SPISOR_FILE_UNKNOWN;
}

static FILE *open_for_reading(const char *path, int *out_of_memory) {
    return spisor_windows_open(path, 0, out_of_memory);
}

static int is_directory(const char *path, int *out_of_memory) {
    *out_of_memory = 0;
    if (!spisor_is_convertible_path(path)) {
        return 0;
    }
    wchar_t *wide = spisor_wide_path(path);
    if (wide == NULL) {
        *out_of_memory = 1;
        return 0;
    }
    DWORD attributes = GetFileAttributesW(wide);
    spisor_memory_release(wide);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}
static FILE *create_temporary(const char *temporary, const char *target, int owner_only, int *out_of_memory) {
    (void)target;
    (void)owner_only;
    return spisor_windows_open(temporary, 1, out_of_memory);
}

static int is_wide_directory(const wchar_t *wide) {
    DWORD attributes = GetFileAttributesW(wide);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static int is_wide_separator(wchar_t character) {
    return character == L'\\' || character == L'/';
}

/** Makes the folder wide names and every missing parent; whether a folder stands there afterwards. */
static int make_wide_directories(wchar_t *wide) {
    if (is_wide_directory(wide)) {
        return 1;
    }
    for (size_t index = 1; wide[index] != L'\0'; index++) {
        if (is_wide_separator(wide[index])) {
            wchar_t separator = wide[index];
            wide[index] = L'\0';
            CreateDirectoryW(wide, NULL);
            wide[index] = separator;
        }
    }
    CreateDirectoryW(wide, NULL);
    return is_wide_directory(wide);
}

static wchar_t *wide_of(const char *path, int *out_of_memory) {
    *out_of_memory = 0;
    if (!spisor_is_convertible_path(path)) {
        return NULL;
    }
    wchar_t *wide = spisor_wide_path(path);
    *out_of_memory = wide == NULL;
    return wide;
}

/** @implNote Every folder is made from the one wide path, so memory running out leaves no folder made. */
spisor_status spisor_make_directories(const char *path) {
    int out_of_memory;
    wchar_t *wide = wide_of(path, &out_of_memory);
    if (wide == NULL) {
        return out_of_memory ? SPISOR_OUT_OF_MEMORY : SPISOR_REFUSED;
    }
    int made = make_wide_directories(wide);
    spisor_memory_release(wide);
    return made ? SPISOR_OK : SPISOR_REFUSED;
}

/** @implNote The parent folders are made from the wide path the file opens by, so running out makes none. */
static FILE *open_for_appending(const char *path, int *out_of_memory) {
    wchar_t *wide = wide_of(path, out_of_memory);
    if (wide == NULL) {
        return NULL;
    }
    size_t last = 0;
    for (size_t index = 1; wide[index] != L'\0'; index++) {
        last = is_wide_separator(wide[index]) ? index : last;
    }
    if (last > 0) {
        wchar_t separator = wide[last];
        wide[last] = L'\0';
        make_wide_directories(wide);
        wide[last] = separator;
    }
    FILE *file = _wfopen(wide, L"ab");
    spisor_memory_release(wide);
    return file;
}

static int move_over(const char *from, const char *to, int *out_of_memory) {
    return spisor_windows_move_over(from, to, out_of_memory);
}

static char *to_utf8(const wchar_t *wide) {
    int length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (length <= 0) {
        return NULL;
    }
    char *narrow = spisor_memory_allocate((size_t)length);
    if (narrow != NULL && WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow, length, NULL, NULL) <= 0) {
        spisor_memory_release(narrow);
        return NULL;
    }
    return narrow;
}

/** A drive-letter path loses its extended-length prefix; a UNC path keeps it, which the wide APIs accept. */
static const wchar_t *without_extended_prefix(const wchar_t *final_path) {
    int has_drive_prefix = wcsncmp(final_path, L"\\\\?\\", 4) == 0 && final_path[4] != 0 && final_path[5] == L':';
    return has_drive_prefix ? final_path + 4 : final_path;
}

static char *final_path_of_link(const wchar_t *wide, int *out_of_memory) {
    HANDLE handle = CreateFileW(wide, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    DWORD needed = GetFinalPathNameByHandleW(handle, NULL, 0, FILE_NAME_NORMALIZED);
    wchar_t *final_path = needed == 0 ? NULL : spisor_memory_allocate((size_t)needed * sizeof(wchar_t));
    char *resolved = NULL;
    *out_of_memory = needed != 0 && final_path == NULL;
    if (final_path != NULL && GetFinalPathNameByHandleW(handle, final_path, needed, FILE_NAME_NORMALIZED) < needed) {
        resolved = to_utf8(without_extended_prefix(final_path));
        *out_of_memory = resolved == NULL;
    }
    spisor_memory_release(final_path);
    CloseHandle(handle);
    return resolved;
}

int spisor_is_link(const char *path, int *out_of_memory) {
    *out_of_memory = 0;
    if (!spisor_is_convertible_path(path)) {
        return 0;
    }
    wchar_t *wide = spisor_wide_path(path);
    if (wide == NULL) {
        *out_of_memory = 1;
        return 0;
    }
    DWORD attributes = GetFileAttributesW(wide);
    spisor_memory_release(wide);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

/** The file a write to path lands in, freed: path itself, or the final target of a link; NULL when none. */
static char *write_target_of(const char *path, int *out_of_memory) {
    *out_of_memory = 0;
    if (!spisor_is_convertible_path(path)) {
        return NULL;
    }
    wchar_t *wide = spisor_wide_path(path);
    if (wide == NULL) {
        *out_of_memory = 1;
        return NULL;
    }
    DWORD attributes = GetFileAttributesW(wide);
    int is_link = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    char *target = is_link ? final_path_of_link(wide, out_of_memory) : duplicate_text(path);
    *out_of_memory = *out_of_memory || (!is_link && target == NULL);
    spisor_memory_release(wide);
    return target;
}
#else
spisor_file_presence spisor_probe_file(const char *path) {
    struct stat status;
    if (stat(path, &status) == 0) {
        return SPISOR_FILE_PRESENT;
    }
    return errno == ENOENT || errno == ENOTDIR ? SPISOR_FILE_MISSING : SPISOR_FILE_UNKNOWN;
}

static FILE *open_for_reading(const char *path, int *out_of_memory) {
    *out_of_memory = 0;
    return fopen(path, "rb");
}

static int is_directory(const char *path, int *out_of_memory) {
    struct stat status;
    *out_of_memory = 0;
    return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
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

static FILE *create_temporary(const char *temporary, const char *target, int owner_only, int *out_of_memory) {
    int exact;
    *out_of_memory = 0;
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

static int is_separator(char character) {
    return character == '/' || character == '\\';
}

/** Makes the folder copy names and every missing parent, copy changed only while it does. */
static void make_directories_of(char *copy) {
    size_t length = strlen(copy);
    for (size_t index = 1; index < length; index++) {
        if (is_separator(copy[index])) {
            char separator = copy[index];
            copy[index] = '\0';
            mkdir(copy, 0777);
            copy[index] = separator;
        }
    }
    mkdir(copy, 0777);
}

spisor_status spisor_make_directories(const char *path) {
    int out_of_memory;
    if (is_directory(path, &out_of_memory)) {
        return SPISOR_OK;
    }
    char *copy = duplicate_text(path);
    if (copy == NULL) {
        return SPISOR_OUT_OF_MEMORY;
    }
    make_directories_of(copy);
    spisor_memory_release(copy);
    return is_directory(path, &out_of_memory) ? SPISOR_OK : SPISOR_REFUSED;
}

static FILE *open_for_appending(const char *path, int *out_of_memory) {
    char *parent = duplicate_text(path);
    *out_of_memory = parent == NULL;
    if (parent == NULL) {
        return NULL;
    }
    size_t last = 0;
    for (size_t index = 1; parent[index] != '\0'; index++) {
        last = is_separator(parent[index]) ? index : last;
    }
    if (last > 0) {
        parent[last] = '\0';
        make_directories_of(parent);
    }
    spisor_memory_release(parent);
    return fopen(path, "ab");
}

static int move_over(const char *from, const char *to, int *out_of_memory) {
    *out_of_memory = 0;
    return rename(from, to) == 0;
}

int spisor_is_link(const char *path, int *out_of_memory) {
    struct stat status;
    *out_of_memory = 0;
    return lstat(path, &status) == 0 && S_ISLNK(status.st_mode);
}

#ifndef PATH_MAX
#define PATH_MAX 4096 /* POSIX lets limits.h leave it out */
#endif

/** @implNote realpath into a buffer, since realpath(path, NULL) answers memory the binary's allocator never saw. */
static char *resolved_link(const char *path, int *out_of_memory) {
    char resolved[PATH_MAX];
    if (realpath(path, resolved) == NULL) {
        return NULL;
    }
    char *copy = duplicate_text(resolved);
    *out_of_memory = copy == NULL;
    return copy;
}

/** The file a write to path lands in, freed: path itself, or the final target of a link; NULL when none. */
static char *write_target_of(const char *path, int *out_of_memory) {
    if (spisor_is_link(path, out_of_memory)) {
        return resolved_link(path, out_of_memory);
    }
    char *copy = duplicate_text(path);
    *out_of_memory = copy == NULL;
    return copy;
}
#endif

static uint8_t *grown_block(uint8_t *bytes, size_t used, size_t capacity) {
    uint8_t *grown = spisor_memory_allocate(capacity);
    if (grown != NULL) {
        memcpy(grown, bytes, used);
    }
    return grown;
}

static uint8_t *read_stream(FILE *file, size_t *length, spisor_read_outcome *outcome) {
    size_t capacity = 4096;
    size_t used = 0;
    uint8_t *bytes = spisor_memory_allocate(capacity);
    *outcome = bytes == NULL ? SPISOR_READ_OUT_OF_MEMORY : SPISOR_READ_FAILED;
    while (bytes != NULL) {
        if (used + 1 >= capacity) {
            uint8_t *grown = grown_block(bytes, used, capacity * 2);
            if (grown == NULL) {
                *outcome = SPISOR_READ_OUT_OF_MEMORY;
                break;
            }
            spisor_memory_release(bytes);
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
            *outcome = SPISOR_READ_DONE;
            return bytes;
        }
    }
    spisor_memory_release(bytes);
    return NULL;
}

uint8_t *spisor_read_file_reporting(const char *path, size_t *length, spisor_read_outcome *outcome) {
    int out_of_memory;
    *outcome = SPISOR_READ_FAILED;
    if (path[0] == '\0') {
        return NULL;
    }
    int directory = is_directory(path, &out_of_memory);
    FILE *file = out_of_memory || directory ? NULL : open_for_reading(path, &out_of_memory);
    if (file == NULL) {
        *outcome = out_of_memory ? SPISOR_READ_OUT_OF_MEMORY : SPISOR_READ_FAILED;
        return NULL;
    }
    uint8_t *bytes = read_stream(file, length, outcome);
    fclose(file);
    return bytes;
}

uint8_t *spisor_read_file(const char *path, size_t *length) {
    spisor_read_outcome outcome;
    return spisor_read_file_reporting(path, length, &outcome);
}

static int write_and_close(FILE *file, const uint8_t *bytes, size_t length) {
    int written = fwrite(bytes, 1, length, file) == length;
    return fclose(file) == 0 && written;
}

static void seed_once(void) {
    static int seeded = 0;
    if (!seeded) {
        int local = 0;
        srand((unsigned)time(NULL) ^ (unsigned)(size_t)&local);
        seeded = 1;
    }
}

char *spisor_temporary_path_for(const char *path) {
    seed_once();
    size_t path_length = strlen(path);
    char *temporary = spisor_memory_allocate(path_length + 1 + 16 + 4 + 1);
    spisor_file_presence presence = SPISOR_FILE_PRESENT;
    while (temporary != NULL && presence == SPISOR_FILE_PRESENT) {
        snprintf(temporary, path_length + 1 + 16 + 4 + 1, "%s.%04x%04x%04x%04x.tmp", path,
                 (unsigned)rand() & 0xffffu, (unsigned)rand() & 0xffffu, (unsigned)rand() & 0xffffu,
                 (unsigned)rand() & 0xffffu);
        presence = spisor_probe_file(temporary);
    }
    if (presence == SPISOR_FILE_OUT_OF_MEMORY) {
        spisor_memory_release(temporary);
        return NULL;
    }
    return temporary;
}

FILE *spisor_open_replacement(const char *target, int owner_only, spisor_prepared_removal *temporary,
                              int *out_of_memory) {
    char *name = spisor_temporary_path_for(target);
    spisor_status prepared = name == NULL ? SPISOR_OUT_OF_MEMORY : spisor_prepare_removal(name, temporary);
    spisor_memory_release(name);
    *out_of_memory = prepared == SPISOR_OUT_OF_MEMORY;
    FILE *file = prepared != SPISOR_OK ? NULL : create_temporary(temporary->path, target, owner_only, out_of_memory);
    if (file == NULL && prepared == SPISOR_OK) {
        spisor_release_prepared_removal(temporary);
    }
    return file;
}

static spisor_status refused_unless_out_of_memory(int out_of_memory) {
    return out_of_memory ? SPISOR_OUT_OF_MEMORY : SPISOR_REFUSED;
}

spisor_status spisor_append_file(const char *path, const uint8_t *bytes, size_t length) {
    int out_of_memory;
    FILE *file = open_for_appending(path, &out_of_memory);
    if (file == NULL) {
        return refused_unless_out_of_memory(out_of_memory);
    }
    return write_and_close(file, bytes, length) ? SPISOR_OK : SPISOR_REFUSED;
}

static spisor_status replace_resolved_file(const char *target, const uint8_t *bytes, size_t length, int owner_only) {
    spisor_prepared_removal temporary;
    int out_of_memory;
    FILE *file = spisor_open_replacement(target, owner_only, &temporary, &out_of_memory);
    if (file == NULL) {
        return refused_unless_out_of_memory(out_of_memory);
    }
    out_of_memory = 0;
    int replaced = write_and_close(file, bytes, length) && move_over(temporary.path, target, &out_of_memory);
    if (!replaced) {
        spisor_remove_prepared(&temporary);
    }
    spisor_release_prepared_removal(&temporary);
    return replaced ? SPISOR_OK : refused_unless_out_of_memory(out_of_memory);
}

spisor_status spisor_replace_file(const char *path, const uint8_t *bytes, size_t length) {
    int out_of_memory;
    char *target = write_target_of(path, &out_of_memory);
    if (target == NULL) {
        return refused_unless_out_of_memory(out_of_memory);
    }
    spisor_status status = replace_resolved_file(target, bytes, length, 0);
    spisor_memory_release(target);
    return status;
}

spisor_status spisor_replace_private_file(const char *path, const uint8_t *bytes, size_t length) {
    int out_of_memory;
    int linked = spisor_is_link(path, &out_of_memory);
    if (out_of_memory) {
        return SPISOR_OUT_OF_MEMORY;
    }
    return linked ? SPISOR_REFUSED : replace_resolved_file(path, bytes, length, 1);
}

int spisor_remove_set_aside(const char *path) {
#if defined(_WIN32)
    return spisor_windows_remove_set_aside(path);
#else
    (void)path;
    return 0;
#endif
}
