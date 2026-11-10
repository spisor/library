#ifndef RUNTIME_INSTALL_BY_ID_H
#define RUNTIME_INSTALL_BY_ID_H

#include <stddef.h>
#include <stdint.h>

#include "export.h"

/**
 * install.byId: {id, cache, pluginDir, version?, lastKnown?, acceptPrimary?, apiBase?} installs the plugin the repo id
 * names into cache, deploys it into pluginDir under its import name, and answers {id, name, fullName, version,
 * commit, sha256, ids, primary, primaryConfirmed}. id, name and fullName are those of the repo it was installed
 * from, which is the verified primary and so can differ from the id asked for. commit is the commit the index states
 * for the version ("" when it states none), sha256 the digest of the release index, ids and primary the repo's
 * verified set. version is the index's latest when omitted. apiBase is honoured only as identity.resolve honours it.
 * A local id is refused: it has no host to install from.
 *
 * @implNote A new version comes only from the primary: when identity.alsoAt confirms a primary other than the id
 * asked for, that repo is resolved and its release is read, never the mirror's. The release index is the
 * weivel-index.json asset of the latest release, found through repos/<owner/name>/releases/latest, which is a lookup
 * by name; the repo is resolved again once the index is read and a different owner/name is a refusal, so a name
 * taken in between cannot substitute its index. Each file the index lists is checked against the index's digest by
 * cache.install, and a file url that leaves the index's own directory is refused as any catalog's is.
 *
 * Copies that name different primaries stop the install. When the set cannot be read, or names a primary no copy
 * confirms, the install goes on with the id it was given as the primary and says primaryConfirmed false.
 *
 * The set and primary are kept in <cache>/identities/<folder>.json under the id asked for and read on every install.
 * A primary that differs from the recorded one installs nothing and answers {id, name, fullName, ids, primary,
 * primaryConfirmed, recordedPrimary, needsUserAcceptance: true}, unless acceptPrimary names the new primary.
 */
RUNTIME_EXPORT int32_t runtime_install_by_id(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
