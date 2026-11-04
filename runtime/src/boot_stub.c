#include "boot_stub.h"

#include "paths.h"
#include "spisor/path.h"

#if defined(_WIN32)
#define BOOT_STUB_NAME "forebay-boot.exe"
#else
#define BOOT_STUB_NAME "forebay-boot"
#endif

char *runtime_boot_stub_path(const char *cache) {
    const char *segments[3] = {cache, "boot", BOOT_STUB_NAME};
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 3);
}
