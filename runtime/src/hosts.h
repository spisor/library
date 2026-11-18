#ifndef RUNTIME_HOSTS_H
#define RUNTIME_HOSTS_H

#include "cJSON.h"

/**
 * @file
 * The hosts service's functions, for runtime's own callers: each answers its result, or NULL with *refusal set to
 * {message, fix}, or with it NULL when memory runs out. hosts' list is registry's, runtime_registry_list.
 */

/** What hosts' register registers under the state place: the identity, the provenance and the environment. */
typedef struct runtime_hosts_registration_request {
    const char *state;
    const cJSON *identity;
    const cJSON *provenance;
    const cJSON *environment;
} runtime_hosts_registration_request;

/**
 * hosts' register: answering { provenance, commands, refusedCommands }, what every start of a host does. identity is
 * { id, version, home?, application?, requiresCommand, commands: [{ name, target, arguments }], start? },
 * start being { runtime, entry }, how a launcher starts the host from its bundle. The host is registered
 * keeping the provenance of its first start, then each command is requested; commands answers the
 * names it holds, and refusedCommands { name, detail, fix } the requests refused for a host that does
 * not require a command. A host that requires one is refused instead.
 *
 * @implNote An environment without a user home is refused before anything is registered when identity
 * asks for a command, since the shims' PATH entry is written into that home.
 */
cJSON *runtime_hosts_register(const runtime_hosts_registration_request *request, cJSON **refusal);

/**
 * Called with remove's answer as it stands, holding reversed and drifted, right before the registration is forgotten;
 * 0 stops the removal there as memory running out, the registration kept.
 */
typedef int (*runtime_hosts_before_forget)(const cJSON *answer, void *context);

/**
 * hosts' remove: the host id under the state and cache places, answering { reversed, drifted, forgotten }. Reverses
 * every change the host recorded, then deletes its references file, its run records and its holders
 * file, then calls before_forget, when not NULL, with context, then forgets its registration.
 *
 * @implNote Each step runs only after the one before succeeded and a refusal stops there, so a reversal
 * that fails leaves the registration, and with it the record of what is left to undo, for another
 * attempt; the registration goes last, so a failure before it leaves everything for the next attempt.
 */
cJSON *runtime_hosts_remove(const char *state, const char *cache, const char *id,
                            runtime_hosts_before_forget before_forget, void *context, cJSON **refusal);

/** What hosts' provenance records: origin { method, package?, options?, version? }, and now when has_now is set. */
typedef struct runtime_hosts_provenance_request {
    const cJSON *origin;
    const char *binary;
    const char *home;
    const cJSON *environment;
    int has_now;
    double now;
} runtime_hosts_provenance_request;

/**
 * hosts' provenance: answering the provenance an application's registration records: origin's
 * members, the location, this build's os and architecture as Node names them, and installedAt, now
 * (milliseconds since the epoch, the current time when not given) as toISOString writes it.
 *
 * @implNote The location is the binary on the PATH of environment when there is one, else home, since
 * an install method reports no path.
 */
cJSON *runtime_hosts_provenance(const runtime_hosts_provenance_request *request, cJSON **refusal);

#endif
