#ifndef RUNTIME_MARKETPLACE_BUILD_H
#define RUNTIME_MARKETPLACE_BUILD_H

#include <stddef.h>
#include <stdint.h>

#include "export.h"

/**
 * marketplace.build: {list, built, apiBase?} answers {copy}, the generated copy of a marketplace list that
 * marketplace.schema.json describes: each entry carries the name, fullName and lastKnown identity.resolve answers
 * for its id, the list's alias in place of the name when one is set, and the list's description and categories.
 * built is the RFC 3339 time the copy states, taken from the caller so the same list and answers build the same
 * copy. apiBase is honoured only as identity.resolve honours it.
 *
 * Refuses, with a fix, a list that breaks its schema, a built that is not RFC 3339, an id that is local, below a repo,
 * listed twice or that does not resolve, a repo name that is no import name and has no alias, and two entries
 * of one name once aliases apply, naming both ids. The shape, local, below-a-repo and duplicate refusals come before
 * any lookup.
 *
 * @implNote Names are compared as written, since an import name is lowercase. A repo name outside the import name
 * grammar is refused rather than altered, because the copy schema holds a name to that grammar and an altered name
 * would not be the repo's.
 */
RUNTIME_EXPORT int32_t runtime_marketplace_build(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
