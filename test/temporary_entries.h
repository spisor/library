#ifndef SPISOR_TEST_TEMPORARY_ENTRIES_H
#define SPISOR_TEST_TEMPORARY_ENTRIES_H

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#endif

/** The number of entries ending in .tmp directly inside directory, or -1 when it cannot be listed. */
static inline int count_temporary_entries(const char *directory) {
    int count = 0;
#if defined(_WIN32)
    char pattern[1024];
    wchar_t wide_pattern[1024];
    snprintf(pattern, sizeof pattern, "%s/*.tmp", directory);
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pattern, -1, wide_pattern, 1024) == 0) {
        return -1;
    }
    WIN32_FIND_DATAW found;
    HANDLE handle = FindFirstFileW(wide_pattern, &found);
    if (handle == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        count++;
    } while (FindNextFileW(handle, &found));
    FindClose(handle);
#else
    DIR *handle = opendir(directory);
    if (handle == NULL) {
        return -1;
    }
    struct dirent *entry;
    while ((entry = readdir(handle)) != NULL) {
        size_t name_length = strlen(entry->d_name);
        if (name_length >= 4 && strcmp(entry->d_name + name_length - 4, ".tmp") == 0) {
            count++;
        }
    }
    closedir(handle);
#endif
    return count;
}

#endif
