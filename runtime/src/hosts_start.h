#ifndef RUNTIME_HOSTS_START_H
#define RUNTIME_HOSTS_START_H

#include "cJSON.h"
#include "hosts_layers.h"

/** How many turns of a held host's process timer may fail in a row before it stops, since it can no longer tell whether anyone holds it. */
#define RUNTIME_PRUNE_FAILURES_BEFORE_EXIT 6

/**
 * How often a session host calls updates.check.
 *
 * @implNote Hourly is safe because updates.check itself skips a source asked within its updates.intervalHours.
 */
#define RUNTIME_UPDATE_CHECK_MILLISECONDS 3600000.0

/**
 * What hosts.start starts: the host host_id in environment, an object of variable names to values, beside the
 * language layer's bundle and the libraries it loaded, an object of library id to version; session is not 0 for
 * the application's session host, and in_process not 0 for a session host that serves the process it runs in alone,
 * whose refusal is never published to the loader's ready record, since a launch waiting on that record waits on
 * another process; layer_language is the language whose plugins the start plans, and action_plugin the plugin an
 * ACTION start runs an action of, NULL for any other start.
 */
typedef struct runtime_hosts_start_request {
    const cJSON *environment;
    const char *host_id;
    const char *layer_id;
    const char *layer_version;
    const cJSON *libraries;
    int session;
    int in_process;
    const char *layer_language;
    const char *action_plugin;
} runtime_hosts_start_request;

/**
 * hosts.start: the whole start sequence of a host that runs its plugins in a language layer. Answers
 * {places, identity: {version, home, requiresCommand,
 * application?}, settings, plan, reusedRecordedRun, timers?: {recordRunMilliseconds, pruneFailuresBeforeExit,
 * updateCheckMilliseconds}, noColor, runId}: the places environment resolves to; the identity hostId's registration
 * records, refused with the reinstall fix unless it carries a string version and home, its version being
 * the one the plan runs hostId at; the ecosystem settings of that home; and the plan updates.apply answers
 * with running naming hostId, the layer and each library at the version it runs. A start that is not the
 * session start, or is an in-process one, reuses instead the run updates.recorded answers while a live run
 * record names hostId, with a run record of its own and as plugins that run less the running ids, and then
 * answers no next or rollback. The plan's plugins are then narrowed to those the catalog lists in layer_language, an
 * unlisted one running in no layer, and for an action_plugin to it and every plugin of the layer providing,
 * transitively, a capability it consumes; a layer_language the catalog lists no language-host item for is refused,
 * naming it, an action_plugin of another language is refused, naming its language, and a narrowed plugin consuming a
 * capability that no plugin of the layer provides and a plugin of another language does is refused, naming both. runId
 * is the identity the run publishes its ready record and holds its holders under, as runtime_run_identity answers it
 * for the front door runtime_read_loader_layers reads.
 * timers are answered to the session start alone, its update check 0 unless runId is hostId, so one layer of a session
 * runs updates.check. noColor is whether environment asks for no colour, as runtime_environment_asks_no_color answers
 * it. A refused session start publishes its refusal to the ready record of runId, or of hostId before runId is known,
 * as processes.ready does, once its places have resolved. An answer that cannot be built removes the run record its
 * plan wrote.
 */
cJSON *runtime_hosts_start(const runtime_hosts_start_request *request, cJSON **refusal);

/** Removes the run record plan, a start's plan, names, allocating nothing. */
void runtime_hosts_start_undo(const cJSON *plan);

#endif
