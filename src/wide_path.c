#include "wide_path.h"

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
    wchar_t *wide = malloc((size_t)length * sizeof(wchar_t));
    if (wide != NULL && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, length) <= 0) {
        free(wide);
        return NULL;
    }
    return wide;
}

static wchar_t *full_path_of(const wchar_t *wide) {
    DWORD needed = GetFullPathNameW(wide, 0, NULL, NULL);
    wchar_t *full = needed == 0 ? NULL : malloc((size_t)needed * sizeof(wchar_t));
    if (full != NULL && GetFullPathNameW(wide, needed, full, NULL) >= needed) {
        free(full);
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
    wchar_t *path = malloc(length * sizeof(wchar_t));
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
    wchar_t *full = full_path_of(wide);
    if (full == NULL || wcslen(full) < SHORT_PATH_LIMIT) {
        free(full);
        return wide;
    }
    free(wide);
    wchar_t *extended = prefixed(full);
    free(full);
    return extended;
}
#else
typedef int spisor_wide_path_is_windows_only;
#endif
