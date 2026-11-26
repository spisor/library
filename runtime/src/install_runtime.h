#ifndef RUNTIME_INSTALL_RUNTIME_H
#define RUNTIME_INSTALL_RUNTIME_H

#include "cJSON.h"

/**
 * The JavaScript runtime for platform, as install's runtime answers it; NULL with *refusal set when no installed
 * javascript-host carries a Node pin, or there is none to reuse and the pin cannot be acquired, and NULL alone when
 * memory ran out, then never acquiring one. environment is the PATH the runtime is looked for on.
 */
cJSON *runtime_acquire_javascript(const char *cache, const char *platform, const cJSON *environment, cJSON **refusal);

/**
 * The executable of the JavaScript runtime <cache>/runtimes/javascript.json records, freed. When
 * no record names one that is still a file, as after the PATH's node was upgraded or removed, the runtime
 * is acquired again as install's runtime acquires it, on environment's PATH for this machine, and recorded.
 * NULL with *refusal set when none can be found or acquired, and NULL alone when memory ran out.
 */
char *runtime_javascript_executable(const char *cache, const cJSON *environment, cJSON **refusal);

/**
 * install's runtime: { cache, environment, platform? } answering { name, version, executable, fetched, source },
 * in spec 5.3's order, by the Node pin the host.json of the javascript-host version <cache>/hosts hints carries,
 * node {pinned, minimum, builds: {<platform>: {url, sha256}}}: a node on the PATH of environment of the pin's
 * minimum or newer, run once with -p process.versions.node under a bounded wait (source "path"), else the
 * pinned build for platform (this machine's by default) at <cache>/runtimes/node/<pinned>/<platform>/ (source
 * "acquired"), fetched and verified against its pinned SHA-256 only when it is not there intact, and
 * unpacked by the OS's own tar (bsdtar on Windows, which reads zip). Only the executable is kept, beside a
 * .unpacked marker holding the archive's hash and the executable's, and every other version under
 * <cache>/runtimes/node is removed. The answer is recorded in <cache>/runtimes/javascript.json, the
 * runtime every host started on JavaScript runs with. Refused when no javascript-host is hinted or its host.json
 * pins no Node.
 *
 * @implNote The pin is the item's catalog data, written by stage-bundle javascript-host, so moving Node needs a new
 * javascript-host and no runtime release. POSIX takes tar.gz because every system tar reads gzip, while xz and unzip
 * are missing from a minimal Linux. The runtime is built in a staging folder of this process and renamed
 * into place, so two installs acquiring at once never unpack into one folder; a PATH node is only looked
 * for on this machine's own platform.
 */
cJSON *runtime_install_runtime(const char *cache, const char *platform, const cJSON *environment, cJSON **refusal);

#endif
