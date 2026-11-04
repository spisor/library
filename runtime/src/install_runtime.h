#ifndef RUNTIME_INSTALL_RUNTIME_H
#define RUNTIME_INSTALL_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "export.h"

/**
 * The JavaScript runtime for platform, as install.runtime answers it; NULL with *refusal set when there is
 * none to reuse and the pin cannot be acquired. environment is the PATH the runtime is looked for on.
 */
cJSON *runtime_acquire_javascript(const char *cache, const char *platform, const cJSON *environment, cJSON **refusal);

/**
 * install.runtime: { cache, environment, platform? } answering { name, version, executable, fetched, source },
 * in spec 5.3's order: a node on the PATH of environment whose major version is 20 or more, run once with
 * -p process.versions.node under a bounded wait (source "path"), else the pinned Node 26.13.0 for
 * platform (this machine's by default) at <cache>/runtimes/node/<version>/<platform>/ (source
 * "acquired"), fetched and verified against its pinned SHA-256 only when it is not there intact, and
 * unpacked by the OS's own tar (bsdtar on Windows, which reads zip). Only the executable is kept, beside a
 * .unpacked marker holding the archive's hash and the executable's, and every other version under
 * <cache>/runtimes/node is removed. The answer is recorded in <cache>/runtimes/javascript.json, the
 * runtime every host started on JavaScript runs with.
 *
 * @implNote Each pin's sha256 is copied from https://nodejs.org/dist/v26.13.0/SHASUMS256.txt, never
 * computed from a download. POSIX takes tar.gz because every system tar reads gzip, while xz and unzip
 * are missing from a minimal Linux. The runtime is built in a staging folder of this process and renamed
 * into place, so two installs acquiring at once never unpack into one folder; a PATH node is only looked
 * for on this machine's own platform.
 */
RUNTIME_EXPORT int32_t runtime_install_runtime(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
