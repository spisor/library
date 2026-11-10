#ifndef RUNTIME_INSTALL_BY_ID_H
#define RUNTIME_INSTALL_BY_ID_H

#include <stddef.h>
#include <stdint.h>

#include "export.h"

/**
 * install.byId: {id, cache, pluginDir, version?, lastKnown?, apiBase?, recordedPrimary?} installs the plugin the repo
 * id names into cache, deploys it into pluginDir under its import name, and answers {id, name, fullName, version,
 * commit, sha256, ids, primary, primaryConfirmed}. name is the repo's name now, fullName its owner/name, commit the
 * commit the index states for the version ("" when it states none), sha256 the digest of the release index, and
 * ids and primary the repo's verified set. version is the index's latest when omitted. apiBase is honoured only as
 * identity.resolve honours it. A local id is refused: it has no host to install from.
 *
 * @implNote The install reads the repo's release index, the weivel-index.json asset of its latest release,
 * found through repos/<owner/name>/releases/latest, which is a lookup by name; the id is resolved again once the
 * index is read and a different owner/name is a refusal, so a name taken in between cannot substitute its index.
 * Each file the index lists is checked against the index's digest by cache.install, and a file url that leaves the
 * index's own directory is refused as any catalog's is.
 *
 * The set comes from identity.alsoAt. When that cannot confirm a primary, because the start file was unreadable,
 * or names a primary no copy confirms, or the lookup itself was refused, the install goes on with the id it was
 * given as the primary and says primaryConfirmed false. recordedPrimary is the primary the caller kept at an
 * earlier install: a different one is decided by identity.acceptsVersion, and when that needs the user the answer
 * is {id, name, fullName, ids, primary, primaryConfirmed, recordedPrimary, needsUserAcceptance: true} and nothing
 * is installed.
 */
RUNTIME_EXPORT int32_t runtime_install_by_id(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
