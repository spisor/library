#include "platform_key.h"

#if defined(_WIN32)
#define RUNTIME_OPERATING_SYSTEM "win32"
#elif defined(__APPLE__)
#define RUNTIME_OPERATING_SYSTEM "darwin"
#elif defined(__linux__)
#define RUNTIME_OPERATING_SYSTEM "linux"
#elif defined(__FreeBSD__)
#define RUNTIME_OPERATING_SYSTEM "freebsd"
#elif defined(__OpenBSD__)
#define RUNTIME_OPERATING_SYSTEM "openbsd"
#elif defined(_AIX)
#define RUNTIME_OPERATING_SYSTEM "aix"
#elif defined(__sun)
#define RUNTIME_OPERATING_SYSTEM "sunos"
#else
#error "no platform key is known for this operating system"
#endif

#if defined(_M_X64) || defined(__x86_64__)
#define RUNTIME_ARCHITECTURE "x64"
#elif defined(_M_ARM64) || defined(__aarch64__)
#define RUNTIME_ARCHITECTURE "arm64"
#elif defined(_M_IX86) || defined(__i386__)
#define RUNTIME_ARCHITECTURE "ia32"
#elif defined(_M_ARM) || defined(__arm__)
#define RUNTIME_ARCHITECTURE "arm"
#elif defined(__riscv) && __riscv_xlen == 64
#define RUNTIME_ARCHITECTURE "riscv64"
#elif defined(__loongarch64)
#define RUNTIME_ARCHITECTURE "loong64"
#elif defined(__powerpc64__)
#define RUNTIME_ARCHITECTURE "ppc64"
#elif defined(__s390x__)
#define RUNTIME_ARCHITECTURE "s390x"
#else
#error "no platform key is known for this architecture"
#endif

const char *runtime_platform_key(void) {
    return RUNTIME_OPERATING_SYSTEM "-" RUNTIME_ARCHITECTURE;
}

static int is_key_character(char character) {
    return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
}

static const char *after_run(const char *text) {
    while (is_key_character(*text)) {
        text++;
    }
    return text;
}

int runtime_is_platform_key(const char *key) {
    const char *hyphen = after_run(key);
    if (hyphen == key || *hyphen != '-') {
        return 0;
    }
    const char *end = after_run(hyphen + 1);
    return end != hyphen + 1 && *end == 0;
}
