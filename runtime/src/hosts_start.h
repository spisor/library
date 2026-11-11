#ifndef RUNTIME_HOSTS_START_H
#define RUNTIME_HOSTS_START_H

#include <stddef.h>
#include <stdint.h>

#include "export.h"

/** How many turns of a held host's process timer may fail in a row before it stops, since it can no longer tell whether anyone holds it. */
#define RUNTIME_PRUNE_FAILURES_BEFORE_EXIT 6

/**
 * How often a session host calls updates.check.
 *
 * @implNote Hourly is safe because updates.check itself skips a source asked within its updates.intervalHours.
 */
#define RUNTIME_UPDATE_CHECK_MILLISECONDS 3600000.0

/**
 * hosts.start: {environment, hostId, layer: {id, version}, libraries: {id: version}, session}, the whole
 * start sequence of a host that runs its plugins in a language layer, layer being that layer's bundle and
 * libraries the libraries it loaded. Answers {home, places, identity: {version, home, requiresCommand,
 * application?}, settings, plan, reusedRecordedRun, timers: {recordRunMilliseconds, pruneFailuresBeforeExit,
 * updateCheckMilliseconds}, noColor}: the places environment resolves to; the identity hostId's registration
 * records, refused with the reinstall fix unless it carries a string version and home; the ecosystem
 * settings of that home; and the plan updates.apply answers with running naming hostId, the layer and each
 * library at the version it runs and libraries their ids. A start that is not the session start reuses
 * instead the run updates.recorded answers while a live run record names hostId, with a run record of its
 * own and as plugins that run less the running ids, and then answers no next or rollback. noColor is
 * whether environment carries NO_COLOR at all.
 */
RUNTIME_EXPORT int32_t runtime_hosts_start(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
