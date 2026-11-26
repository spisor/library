#ifndef RUNTIME_MARKETPLACE_MEMORY_H
#define RUNTIME_MARKETPLACE_MEMORY_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

/**
 * What marketplace.remember remembers: that name, in marketplace or bare when it is NULL, was given id and its at
 * places; accept is the id the user accepted in place of the one remembered, accept_at the places the user accepted in
 * place of the remembered ones, joined by commas, each NULL when none was; is_recording 0 only checks. Every text
 * present is not empty, and state is the host's state place.
 */
typedef struct runtime_marketplace_remember_request {
    const char *state;
    const char *marketplace;
    const char *name;
    const char *id;
    const char *const *at;
    size_t at_count;
    const char *accept;
    const char *accept_at;
    int is_recording;
} runtime_marketplace_remember_request;

/**
 * marketplace.remember: answers {remembered: true} once the id and the at places a name was given are the ones this
 * host has on record for it, recording them when the host had none. Answers {needsUserAcceptance: true, was, now,
 * nowAt} and records nothing when the host remembers another id for that name, unless accept is the id now offered,
 * which is then recorded with the places nowAt named in place of the old ones. Answers {needsAtAcceptance: true, wasAt, nowAt} and records
 * nothing when the id is the remembered one and its places are not, in the same order, unless accept_at is exactly the
 * places now offered: a copy that keeps an id and points its places at a copy of the repo would otherwise be trusted on
 * first contact. Not recording it only checks: it answers {settled: true} where it would have recorded or found the
 * same, or the same question, and writes nothing.
 *
 * The record is <state>/marketplace-names.json, {format: 2, names: {"marketplace/name": {id, at}}}, written
 * atomically. Without a marketplace the key is the bare name, which never holds a slash, so a bare name is remembered
 * beside the qualified ones. Refuses a record that is not that shape, a format 1 record included, naming the file,
 * and leaves it as it is.
 *
 * @implNote Keyed by marketplace and name together, so one name in two marketplaces is two memories: the id a marketplace
 * lists for a name is the only thing standing between a user and a name that was quietly pointed somewhere else. The
 * bare key catches a name that moves to another marketplace, which no qualified key can see.
 */
cJSON *runtime_marketplace_remember(const runtime_marketplace_remember_request *request, cJSON **refusal);

#endif
