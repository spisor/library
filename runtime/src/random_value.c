#if defined(__linux__)
#define _GNU_SOURCE
#elif !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "random_value.h"

#if defined(_WIN32)
#include <windows.h>

#include <bcrypt.h>

int runtime_random_64(uint64_t *value) {
    return BCryptGenRandom(NULL, (PUCHAR)value, (ULONG)sizeof *value, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

#elif defined(__linux__)
#include <errno.h>
#include <sys/random.h>

int runtime_random_64(uint64_t *value) {
    ssize_t drawn = -1;
    do {
        drawn = getrandom(value, sizeof *value, 0);
    } while (drawn < 0 && errno == EINTR);
    return drawn == (ssize_t)sizeof *value;
}

#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

int runtime_random_64(uint64_t *value) {
    int source = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    ssize_t drawn = -1;
    if (source < 0) {
        return 0;
    }
    do {
        drawn = read(source, value, sizeof *value);
    } while (drawn < 0 && errno == EINTR);
    close(source);
    return drawn == (ssize_t)sizeof *value;
}

#endif
