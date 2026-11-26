#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "runnable.h"

#if defined(_WIN32)

int runtime_make_runnable(const char *path) {
    (void)path;
    return 1;
}

#else

#include <sys/stat.h>

int runtime_make_runnable(const char *path) {
    struct stat status;
    return stat(path, &status) == 0 && ((status.st_mode & S_IXUSR) != 0 || chmod(path, status.st_mode | S_IXUSR) == 0);
}

#endif
