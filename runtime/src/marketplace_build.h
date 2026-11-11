#ifndef RUNTIME_MARKETPLACE_BUILD_H
#define RUNTIME_MARKETPLACE_BUILD_H

#include <stddef.h>
#include <stdint.h>

#include "export.h"

/**
 * marketplace.build: {list, built, token?, tokenHost?, previous?, apiBase?} answers {copy, skipped}, the generated copy of a marketplace list that
 * marketplace.schema.json describes. Each entry carries the fullName and lastKnown (the same) that identity.resolve
 * answers for the repo its id names, and a name: the list's alias when one is set, otherwise the repo's name, or the
 * last segment of the folder when the id names a folder below the repo, derived as an import name. The list's
 * description and categories are carried. built is the RFC 3339 time the copy states, taken from the caller so the
 * same list and answers build the same copy. apiBase is honoured only as identity.resolve honours it.
 *
 * A name is derived by lowercasing it, turning each run of characters outside [a-z0-9.-] into one "-", reducing every
 * run of separators to its first and trimming "-" and "." at both ends. An entry whose derived name is still no import
 * name (empty, or reserved such as "con" or "package") is left out of the copy and listed in skipped as {id, reason}.
 *
 * An entry whose repository cannot be looked up does not stop the build: when previous, the copy this build replaces,
 * has an entry of that id, that entry is carried over unchanged, and otherwise the entry is left out and listed in
 * skipped with the reason. token is the access token identity.resolve sends to the host the ids name, and only there;
 * it is never written into the copy or a refusal. tokenHost, required with token, is the one host it belongs to:
 * github.com for github ids, or the instance host for gitea and forgejo ids, and a list holding an id on any other
 * host is refused before any lookup. A token that is not usable is refused the same way.
 *
 * A list with at least one entry whose copy would hold none is refused, writing nothing, with the reasons seen
 * (rate limiting, not found, a network failure, an unusable name); an empty list builds an empty copy.
 *
 * Refuses, with a fix, a list that breaks its schema, a built that is not RFC 3339, a previous that is not a copy, an
 * id that is local or listed twice, and two entries of one name once names are derived and aliases apply, naming both
 * ids. The shape, local, duplicate, token and host refusals come before any lookup.
 *
 * @implNote Names are compared as written, since an import name is lowercase. A repo is looked up by the id up to its
 * folder, so a renamed repo never changes the list.
 */
RUNTIME_EXPORT int32_t runtime_marketplace_build(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
