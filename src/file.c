#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "spisor/file.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>
#else
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
#else
int spisor_file_exists(const char *path) {
    struct stat status;
    return path[0] != '\0' && stat(path, &status) == 0;
}

static FILE *open_for_reading(const char *path) {
    return fopen(path, "rb");
}

static int is_directory(const char *path) {
    struct stat status;
    return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
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
