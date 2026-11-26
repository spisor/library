#ifndef RUNTIME_INSTALL_BY_ID_H
#define RUNTIME_INSTALL_BY_ID_H

#include <stddef.h>

#include "cJSON.h"
#include "spisor/status.h"

/** Takes an installed answer into `taken`: OK, or OUT_OF_MEMORY to stop the install before it deploys. */
typedef spisor_status (*runtime_install_by_id_taker)(const cJSON *answer, void *taken);

/**
 * What install's byId installs: the plugin id names, found from its `at` places, into cache, deployed into
 * plugin_directory; version, accept_primary and accept_number are NULL when not known, and every text given is not
 * empty.
 */
typedef struct runtime_install_by_id_request {
    const char *id;
    const char *const *at;
    size_t at_count;
    const char *cache;
    const char *plugin_directory;
    const char *version;
    const char *accept_primary;
    const char *accept_number;
} runtime_install_by_id_request;

/**
 * install's byId: resolves the repo the id names through naula from its `at` places, installs the plugin into cache
 * from the latest release of the repo's primary, deploys it into the plugin directory under its import name, and
 * answers {id, name, fullName, homes, primary, version, commit, sha256, cacheProblem?}. name and fullName are the
 * primary's, homes the places that confirmed the id, commit the commit the index states for the version ("" when it
 * states none), sha256 the digest of the release index, cacheProblem why naula could not keep its confirmations in the
 * cache. version is the index's latest when omitted. A local id is refused: it has no home to install from.
 *
 * @implNote A new version comes only from the primary, which naula confirms: never from a copy, a fork or a name.
 * The release index is the weivel-index.json asset of the primary's latest release, which the primary's provider
 * reads by number, so a name taken in between cannot substitute its index. Each file the index lists is checked
 * against the index's digest by cache.install, relative to the address the release offers the index at, and a file
 * url that leaves the index's own directory is refused as any catalog's is.
 *
 * naula keeps what confirmed the id under the cache's identities and is the record a later install is compared with.
 * A primary other than the one confirmed before installs nothing and answers {id, primary, recordedPrimary,
 * needsUserAcceptance: true} unless acceptPrimary names the new primary; accounts whose numbers the records disagree
 * on install nothing and answer {id, disputes: [{account, recordedNumber, offeredNumber, accept}],
 * needsNumberAcceptance: true} unless acceptNumber lists each one's accept text. A refusal that is neither wins.
 *
 * Hands an installed answer to `take` before the plugin is deployed, the last write, so OUT_OF_MEMORY never follows
 * a deploy; NULL with no refusal when `take` runs out of memory.
 */
cJSON *runtime_install_by_id(const runtime_install_by_id_request *install, runtime_install_by_id_taker take,
                             void *taken, cJSON **refusal);

#endif
