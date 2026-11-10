#ifndef RUNTIME_IDENTITY_RESOLVE_H
#define RUNTIME_IDENTITY_RESOLVE_H

#include <stddef.h>
#include <stdint.h>

#include "export.h"

/**
 * identity.resolve: {id, lastKnown?, apiBase?} answers {name, fullName, defaultBranch?} for the repo the id names,
 * or refuses with a fix. lastKnown is an "owner/name" a previous lookup saw. apiBase replaces the host's API root,
 * is not in library.json, and is refused unless the fetch test hook allows plain http, so only a test can aim a lookup.
 *
 * @implNote A repo is looked up by its number, which survives a rename; a number the host answers with is
 * always compared, as text, with the one asked for. Only when the host cannot answer by number (404, or 401 where
 * it wants a token for that route) is lastKnown asked, and its answer is accepted only when it carries the same
 * number, so a name someone else has since taken is refused. A local id has no host and answers from its own text.
 */
RUNTIME_EXPORT int32_t runtime_identity_resolve(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
