#if !defined(_WIN32) && !defined(__APPLE__)
#define _POSIX_C_SOURCE 200809L
#endif

#include "file_times.h"

#include <stdlib.h>
#include <string.h>

#include "directory_tree.h"
#include "paths.h"
#include "spisor/path.h"

#if defined(_WIN32)
#include <windows.h>

#include "wide_path.h"
#else
#include <sys/stat.h>
#include <time.h>
#endif

#if defined(_WIN32)

#define EPOCH_IN_FILETIME_TICKS 116444736000000000ULL

static double milliseconds_of(FILETIME time) {
    ULONGLONG ticks = ((ULONGLONG)time.dwHighDateTime << 32) | time.dwLowDateTime;
    return ((double)ticks - (double)EPOCH_IN_FILETIME_TICKS) / 10000.0;
}

double runtime_now_milliseconds(void) {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    return milliseconds_of(now);
}

/** GetFileAttributesExW describes a link itself, never what it points at, as lstat does. */
static int own_change(const char *path, double *milliseconds, int *is_directory) {
    wchar_t *wide = spisor_wide_path(path);
    WIN32_FILE_ATTRIBUTE_DATA data;
    memset(&data, 0, sizeof data);
    int read = wide != NULL && GetFileAttributesExW(wide, GetFileExInfoStandard, &data) != 0;
    free(wide);
    if (read) {
        *milliseconds = milliseconds_of(data.ftLastWriteTime);
        *is_directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                        (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
    }
    return read;
}

#else

double runtime_now_milliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1000000.0;
}

static int own_change(const char *path, double *milliseconds, int *is_directory) {
    struct stat status;
    if (lstat(path, &status) != 0) {
        return 0;
    }
#if defined(__APPLE__)
    struct timespec modified = status.st_mtimespec;
#else
    struct timespec modified = status.st_mtim;
#endif
    *milliseconds = (double)modified.tv_sec * 1000.0 + (double)modified.tv_nsec / 1000000.0;
    *is_directory = S_ISDIR(status.st_mode);
    return 1;
}

#endif

static int newest_child_change(const char *path, double *newest) {
    runtime_directory_listing listing;
    if (!runtime_list_directory(path, &listing)) {
        return 0;
    }
    int read = 1;
    for (size_t index = 0; read && index < listing.count; index++) {
        const char *segments[2] = {path, listing.entries[index].name};
        char *child = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
        double change = 0;
        read = child != NULL && runtime_newest_change(child, &change);
        if (read && change > *newest) {
            *newest = change;
        }
        free(child);
    }
    runtime_free_directory_listing(&listing);
    return read;
}

int runtime_newest_change(const char *path, double *milliseconds) {
    int is_directory = 0;
    if (!own_change(path, milliseconds, &is_directory)) {
        return 0;
    }
    return !is_directory || newest_child_change(path, milliseconds);
}
