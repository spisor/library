#ifndef RUNTIME_TEST_FILE_AGE_H
#define RUNTIME_TEST_FILE_AGE_H

#include <time.h>

#include "check.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <utime.h>
#endif

/** Sets the modification time of path, a file or a folder, to milliseconds since the epoch, whole seconds kept. */
static inline void set_modified(const char *path, double milliseconds) {
#if defined(_WIN32)
    wchar_t wide[2048];
    CHECK(MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 2048) > 0);
    HANDLE handle = CreateFileW(wide, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    CHECK(handle != INVALID_HANDLE_VALUE);
    ULONGLONG ticks = (ULONGLONG)(milliseconds * 10000.0) + 116444736000000000ULL;
    FILETIME time = {(DWORD)ticks, (DWORD)(ticks >> 32)};
    CHECK(SetFileTime(handle, NULL, &time, &time) != 0);
    CloseHandle(handle);
#else
    struct utimbuf times;
    times.actime = (time_t)(milliseconds / 1000.0);
    times.modtime = times.actime;
    CHECK(utime(path, &times) == 0);
#endif
}

#endif
