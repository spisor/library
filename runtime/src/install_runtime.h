#ifndef RUNTIME_INSTALL_RUNTIME_H
#define RUNTIME_INSTALL_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "export.h"

/**
 * The pinned JavaScript runtime for platform in cache, acquired when it is not unpacked intact, as
 * install.runtime answers it; NULL with *refusal set when it cannot be.
 */
cJSON *runtime_acquire_javascript(const char *cache, const char *platform, cJSON **refusal);

/**
 * install.runtime: { cache, platform? } answering { name, version, executable, fetched }: the pinned Node
 * for platform (this machine's by default) at <cache>/runtimes/node/<version>/<platform>/, fetched and
 * verified against its pinned SHA-256 only when it is not there intact, and unpacked by the OS's own tar
 * (bsdtar on Windows, which reads zip). Only the executable is kept, beside a .unpacked marker holding
 * the archive's hash and the executable's, and every other version under <cache>/runtimes/node is
 * removed.
 *
 * @implNote Each pin's sha256 is copied from https://nodejs.org/dist/v26.13.0/SHASUMS256.txt, never
 * computed from a download. POSIX takes tar.gz because every system tar reads gzip, while xz and unzip
 * are missing from a minimal Linux. A version that cannot be removed because it is running is left
 * for the next acquisition.
 */
RUNTIME_EXPORT int32_t runtime_install_runtime(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
