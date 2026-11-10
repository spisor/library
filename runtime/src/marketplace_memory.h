#ifndef RUNTIME_MARKETPLACE_MEMORY_H
#define RUNTIME_MARKETPLACE_MEMORY_H

#include <stddef.h>
#include <stdint.h>

#include "export.h"

/**
 * marketplace.remember: {state, marketplace, name, id, accept?} answers {remembered: true} once the id a marketplace
 * gave a name is the one this host has on record for it, and records it when the host had none. Answers
 * {needsUserAcceptance: true, was, now} and records nothing when the host remembers another id for that name, unless
 * accept is the id now offered, which is then recorded in place of the old one.
 *
 * The record is <state>/marketplace-names.json, {format: 1, names: {"marketplace/name": id}}, written atomically.
 * Refuses a record that is not that shape, naming the file, and leaves it as it is.
 *
 * @implNote Keyed by marketplace and name together, so one name in two marketplaces is two memories: the id a marketplace
 * lists for a name is the only thing standing between a user and a name that was quietly pointed somewhere else.
 */
RUNTIME_EXPORT int32_t runtime_marketplace_remember(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
