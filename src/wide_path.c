#include "wide_path.h"
#include "allocation.h"

#if defined(_WIN32)
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define SHORT_PATH_LIMIT 248

static wchar_t *utf16_of(const char *text) {
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (length <= 0) {
        return NULL;
    }
    wchar_t *wide = spisor_memory_allocate((size_t)length * sizeof(wchar_t));
    if (wide != NULL && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, length) <= 0) {
        spisor_memory_release(wide);
        return NULL;
    }
    return wide;
}

int spisor_is_convertible_path(const char *path) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0) > 0;
}

/** The full form of wide, freed, or NULL when the OS gives none; *out_of_memory set when memory ran out. */
static wchar_t *full_path_of(const wchar_t *wide, int *out_of_memory) {
    DWORD needed = GetFullPathNameW(wide, 0, NULL, NULL);
    wchar_t *full = needed == 0 ? NULL : spisor_memory_allocate((size_t)needed * sizeof(wchar_t));
    *out_of_memory = needed != 0 && full == NULL;
    if (full != NULL && GetFullPathNameW(wide, needed, full, NULL) >= needed) {
        spisor_memory_release(full);
        return NULL;
    }
    return full;
}

static int has_device_prefix(const wchar_t *path) {
    return wcsncmp(path, L"\\\\?\\", 4) == 0 || wcsncmp(path, L"\\\\.\\", 4) == 0;
}

static wchar_t *prefixed(const wchar_t *full) {
    int is_share = wcsncmp(full, L"\\\\", 2) == 0;
    const wchar_t *prefix = is_share ? L"\\\\?\\UNC\\" : L"\\\\?\\";
    const wchar_t *rest = is_share ? full + 2 : full;
    size_t length = wcslen(prefix) + wcslen(rest) + 1;
    wchar_t *path = spisor_memory_allocate(length * sizeof(wchar_t));
    if (path != NULL) {
        wcscpy(path, prefix);
        wcscat(path, rest);
    }
    return path;
}

wchar_t *spisor_wide_path(const char *path) {
    wchar_t *wide = utf16_of(path);
    if (wide == NULL || has_device_prefix(wide)) {
        return wide;
    }
    int out_of_memory;
    wchar_t *full = full_path_of(wide, &out_of_memory);
    if (out_of_memory) {
        spisor_memory_release(wide);
        return NULL;
    }
    if (full == NULL || wcslen(full) < SHORT_PATH_LIMIT) {
        spisor_memory_release(full);
        return wide;
    }
    spisor_memory_release(wide);
    wchar_t *extended = prefixed(full);
    spisor_memory_release(full);
    return extended;
}

/** Writes the extended-length prefix text needs at buffer's start, moving text past what the prefix replaces. */
static wchar_t *after_extended_prefix(wchar_t *buffer, const wchar_t **text) {
    int is_share = wcsncmp(*text, L"\\\\", 2) == 0;
    const wchar_t *prefix = is_share ? L"\\\\?\\UNC" : L"\\\\?\\";
    size_t length = wcslen(prefix);
    wmemcpy(buffer, prefix, length);
    *text += is_share ? 1 : 0;
    return buffer + length;
}

static int is_separator(char character) {
    return character == '/' || character == '\\';
}

/** Whether a segment of path is . or .., which the extended-length prefix would pass to the file system as names. */
static int has_dot_segment(const char *path) {
    const char *segment = path;
    for (const char *cursor = path;; cursor++) {
        if (*cursor != '\0' && !is_separator(*cursor)) {
            continue;
        }
        size_t length = (size_t)(cursor - segment);
        if ((length == 1 || length == 2) && strncmp(segment, "..", length) == 0) {
            return 1;
        }
        if (*cursor == '\0') {
            return 0;
        }
        segment = cursor + 1;
    }
}

int spisor_wide_path_into(const char *path, wchar_t *buffer, size_t capacity) {
    const size_t reserved = 8;
    if (capacity <= reserved || has_dot_segment(path)) {
        return 0;
    }
    wchar_t *text = buffer + reserved;
    int converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, text, (int)(capacity - reserved));
    if (converted <= 0) {
        return 0;
    }
    size_t length = (size_t)converted - 1;
    wchar_t *target = buffer;
    const wchar_t *source = text;
    if (!has_device_prefix(text) && length >= SHORT_PATH_LIMIT) {
        for (size_t index = 0; index < length; index++) {
            text[index] = text[index] == L'/' ? L'\\' : text[index];
        }
        target = after_extended_prefix(buffer, &source);
    }
    wmemmove(target, source, length + 1 - (size_t)(source - text));
    return 1;
}
#else
typedef int spisor_wide_path_is_windows_only;
#endif
