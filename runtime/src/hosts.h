#ifndef RUNTIME_HOSTS_H
#define RUNTIME_HOSTS_H

#include <stddef.h>
#include <stdint.h>

#include "export.h"

/**
 * hosts.register: { places: { state }, identity, provenance, environment } answering
 * { provenance, commands, refusedCommands }, what every start of a host does. identity is
 * { id, version, home?, application?, requiresCommand, commands: [{ name, target, arguments }], start? },
 * start being { runtime, entry }, how a launcher starts the host from its bundle. The host is registered
 * keeping the provenance of its first start, then each command is requested; commands answers the
 * names it holds, and refusedCommands { name, detail, fix } the requests refused for a host that does
 * not require a command. A host that requires one is refused instead.
 *
 * @implNote An environment without a user home is refused before anything is registered when identity
 * asks for a command, since the shims' PATH entry is written into that home.
 */
RUNTIME_EXPORT int32_t runtime_hosts_register(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/** hosts.list: { places: { state } } answering { hosts, unreadable } as registry.list does. */
RUNTIME_EXPORT int32_t runtime_hosts_list(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/**
 * hosts.remove: { places: { state, cache }, id } answering { reversed, drifted, forgotten }. Reverses
 * every change the host recorded, then deletes its references file, its run records and its holders
 * file, then forgets its registration.
 *
 * @implNote Each step runs only after the one before succeeded and a refusal stops there, so a reversal
 * that fails leaves the registration, and with it the record of what is left to undo, for another
 * attempt; the registration goes last, so a failure before it leaves everything for the next attempt.
 */
RUNTIME_EXPORT int32_t runtime_hosts_remove(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/**
 * hosts.provenance: { origin: { method, package?, options?, version? }, application: { binary }, home,
 * environment, now? } answering the provenance an application's registration records: origin's
 * members, the location, this build's os and architecture as Node names them, and installedAt, now
 * (milliseconds since the epoch, the current time by default) as toISOString writes it.
 *
 * @implNote The location is the binary on the PATH of environment when there is one, else home, since
 * an install method reports no path.
 */
RUNTIME_EXPORT int32_t runtime_hosts_provenance(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
