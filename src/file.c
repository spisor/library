#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "spisor/file.h"

#include "file_presence.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <errno.h>
#include <sys/stat.h>
#endif

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

static FILE *open_for_replacing(const char *path) {
    return open_for_writing(path, L"wb");
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

static FILE *open_for_replacing(const char *path) {
    return fopen(path, "wb");
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
    return create_directory(path) && is_directory(path);
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

int spisor_replace_file(const char *path, const uint8_t *bytes, size_t length) {
    char *temporary = temporary_path_for(path);
    if (temporary == NULL) {
        return 0;
    }
    FILE *file = open_for_replacing(temporary);
    int replaced = file != NULL && write_and_close(file, bytes, length) && move_over(temporary, path);
    if (!replaced) {
        remove_file(temporary);
    }
    free(temporary);
    return replaced;
}
