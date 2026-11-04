#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "catalog_path.h"

#include <stdlib.h>
#include <string.h>

#include "paths.h"
#include "spisor/path.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#if defined(_WIN32)
static char *working_directory(void) {
    DWORD length = GetCurrentDirectoryW(0, NULL);
    wchar_t *wide = length == 0 ? NULL : malloc(length * sizeof *wide);
    if (wide == NULL || GetCurrentDirectoryW(length, wide) == 0) {
        free(wide);
        return NULL;
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    char *narrow = size <= 0 ? NULL : malloc((size_t)size);
    if (narrow != NULL && WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow, size, NULL, NULL) != size) {
        free(narrow);
        narrow = NULL;
    }
    free(wide);
    return narrow;
}
#else
static char *working_directory(void) {
    size_t size = 4096;
    for (;;) {
        char *buffer = malloc(size);
        if (buffer == NULL) {
            return NULL;
        }
        if (getcwd(buffer, size) != NULL) {
            return buffer;
        }
        free(buffer);
        if (size > (size_t)1 << 20) {
            return NULL;
        }
        size *= 2;
    }
}
#endif

static int is_separator(char character) {
    return character == '/' || (RUNTIME_HOST_IS_WINDOWS && character == '\\');
}

static int is_root(const char *path, size_t length) {
    if (RUNTIME_HOST_IS_WINDOWS) {
        return length == 3 && path[1] == ':' && is_separator(path[2]);
    }
    return length == 1;
}

static char *without_trailing_separator(char *path) {
    size_t length = strlen(path);
    while (length > 1 && is_separator(path[length - 1]) && !is_root(path, length)) {
        path[--length] = 0;
    }
    return path;
}

static int is_drive_letter_prefix(const char *path) {
    char letter = (char)(path[0] | 0x20);
    return RUNTIME_HOST_IS_WINDOWS && letter >= 'a' && letter <= 'z' && path[1] == ':';
}

/** The path with what it lacks to be absolute: the working directory, or on Windows its drive or a root. */
static char *anchored(const char *path, const char *working) {
    if (!RUNTIME_HOST_IS_WINDOWS || !(is_separator(path[0]) || is_drive_letter_prefix(path))) {
        const char *segments[] = {working, path};
        return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
    }
    char prefix[4] = {working[0], ':', '\\', 0};
    if (is_drive_letter_prefix(path)) {
        prefix[0] = path[0];
        path += 2;
    }
    const char *segments[] = {prefix, path};
    return spisor_path_join(1, segments, 2);
}

char *runtime_catalog_resolve_path(const char *path) {
    if (spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, path) && !(RUNTIME_HOST_IS_WINDOWS && is_separator(path[0]) &&
                                                                   !is_separator(path[1]))) {
        const char *segments[] = {path};
        char *joined = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 1);
        return joined == NULL ? NULL : without_trailing_separator(joined);
    }
    char *working = working_directory();
    if (working == NULL) {
        return NULL;
    }
    char *resolved = anchored(path, working);
    free(working);
    return resolved == NULL ? NULL : without_trailing_separator(resolved);
}
