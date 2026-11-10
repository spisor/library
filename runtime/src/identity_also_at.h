#ifndef RUNTIME_IDENTITY_ALSO_AT_H
#define RUNTIME_IDENTITY_ALSO_AT_H

#include <stddef.h>
#include <stdint.h>

#include "export.h"

/**
 * identity.agree: {id, files} answers {ids, primary, primaryConfirmed, startFileMalformed?, startFileUnavailable?} for
 * the repo id names. files maps a repo id to its also-at file, to null when that copy has none, or to false when
 * reading it failed. Pure: it reads
 * nothing, so a caller holding the files can ask it directly.
 *
 * @implNote This is the only place the two-sided rule lives: an id is counted only when the file of the id we
 * started from lists it and its own file lists the id we started from. Only the ids the starting file lists are
 * considered, never ids those copies list in turn, and a file listing more than 16 ids is malformed. A file that is
 * not an object, or holds an id outside the id grammar, a local id or an id with a folder, is malformed: on another
 * copy that leaves the copy out, as an unreachable one is, and on the starting id it is reported as
 * startFileMalformed and nothing beyond the id itself is counted. A starting file whose read failed (false) is
 * answered with startFileUnavailable and primaryConfirmed false, since a copy that could not be read must not
 * present itself as the primary the way one with no file does. Counted copies naming a primary other than the
 * starting file's are refused, since the set then has no one primary; a named primary no counted copy confirms is
 * answered with primaryConfirmed false.
 */
RUNTIME_EXPORT int32_t runtime_identity_agree(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/**
 * identity.alsoAt: {id, apiBase?} reads `.weivel/also-at.json` at the default branch of the id's repo and of each copy
 * it lists, then answers what identity.agree does. A copy that cannot be reached is left out, no refusal; the
 * id itself being unresolvable is refused. apiBase is honoured only as identity.resolve honours it.
 */
RUNTIME_EXPORT int32_t runtime_identity_also_at(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/**
 * identity.acceptsVersion: {recorded, primary} answers {accepted, needsUserAcceptance}. recorded is the primary
 * kept at install and primary the one a new version comes from; a different one is accepted only by the user.
 */
RUNTIME_EXPORT int32_t runtime_identity_accepts_version(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
