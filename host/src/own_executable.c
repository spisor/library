#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#elif !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "own_executable.h"

#include <stdint.h>
#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

#if defined(_WIN32)

char *host_own_executable(void) {
    wchar_t path[32768];
    DWORD length = GetModuleFileNameW(NULL, path, 32768);
    int bytes = length == 0 || length >= 32768 ? 0 : WideCharToMultiByte(CP_UTF8, 0, path, (int)length, NULL, 0, NULL, NULL);
    char *narrow = bytes <= 0 ? NULL : malloc((size_t)bytes + 1);
    if (narrow != NULL) {
        WideCharToMultiByte(CP_UTF8, 0, path, (int)length, narrow, bytes, NULL, NULL);
        narrow[bytes] = 0;
    }
    return narrow;
}

#elif defined(__APPLE__)

char *host_own_executable(void) {
    uint32_t size = 0;
    _NSGetExecutablePath(NULL, &size);
    char *path = malloc(size + 1);
    if (path != NULL && _NSGetExecutablePath(path, &size) != 0) {
        free(path);
        path = NULL;
    }
    return path;
}

#else

char *host_own_executable(void) {
    size_t capacity = 4096;
    char *path = malloc(capacity);
    ssize_t length = path == NULL ? -1 : readlink("/proc/self/exe", path, capacity - 1);
    if (length < 0) {
        free(path);
        return NULL;
    }
    path[length] = 0;
    return path;
}

#endif
