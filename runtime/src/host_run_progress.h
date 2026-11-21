#ifndef RUNTIME_HOST_RUN_PROGRESS_H
#define RUNTIME_HOST_RUN_PROGRESS_H

#include <stddef.h>
#include <stdint.h>

#include <weivel/host.h>

#include "host_run.h"
#include "spisor/status.h"

/**
 * @file
 * What `weivel.host` does with a run between its start and its stop: `started`, `tick` and `finished` on the run the
 * caller holds under the run lock. Each answers OK or refuses into `refusal`; every answer but OK fails a run that has
 * not ended, its exit code becoming RUNTIME_HOST_EXIT_FAILED, and leaves the exit code of one that has as it was, so a
 * call out of its order after a run ended on its holders' word or with its action's result changes nothing.
 */

/** When the program ticks next: at `next`, milliseconds since the Unix epoch, when `has_next` is 1. */
typedef struct runtime_host_next_tick {
    uint32_t has_next;
    int64_t next;
} runtime_host_next_tick;

/**
 * Takes every plugin's outcome, one per plugin of the plan in its order. A plugin that was refused or failed refuses
 * that plugin alone, as the archived host quarantined it: the run carries on with the plugins that activated. A SERVE
 * run then publishes to the loader's ready record that it is ready, with an empty environment, unless a plugin whose
 * manifest declares the `front-door` capability was not activated: then it publishes a refusal naming that front door.
 *
 * @return OK; MALFORMED for outcomes other than one per plugin of the plan, a manifest that is not the plan's in its
 * place, or a state that is no weivel_standard_host_plugin_state; REFUSED when it is not the run's first started, or
 * when the ready record could not be written; OUT_OF_MEMORY.
 */
spisor_status runtime_host_run_started(runtime_host_run *run, const weivel_standard_host_outcome *outcomes,
                                       size_t outcome_count, spisor_refusal *refusal);

/**
 * Acts on `now` at the run's timers, which hosts.start answered a SERVE run, reading no clock: the run record
 * stamped alive at `now` and the holders' prune at `now` every `recordRunMilliseconds`, and updates.check every
 * `updateCheckMilliseconds`, the first tick taking both.
 * A timer's next due time is `now` plus its interval, so a late tick never runs a timer twice to catch up. `next` is
 * the earlier of the two due times, or none when hosts.start answered no timers.
 *
 * A turn whose stamp or prune is refused counts towards `pruneFailuresBeforeExit` turns in a row, and one that
 * succeeds starts the count again; an updates.check that is refused leaves the run as it was, since what runs now is
 * never changed by a check. Each failed turn, a refused check and each version a check refused are written as a
 * warning to the loader's own log at `now`, under the loader's id.
 *
 * @return OK; REFUSED for an ACTION run, which never ticks, a run whose started has not answered OK, and a now outside
 * the years 0 to 9999 when a check is due; REFUSED with the run ended and its exit code 0 when the prune answers that
 * no holder is alive or another process runs the loader, or after `pruneFailuresBeforeExit` failed turns in a row;
 * OUT_OF_MEMORY.
 *
 * @implNote The update check waits on the network, and the caller holds the run lock throughout, so a stop racing a
 * blocking tick waits for it rather than freeing the run under it.
 */
spisor_status runtime_host_run_tick(runtime_host_run *run, int64_t now, runtime_host_next_tick *next,
                                    spisor_refusal *refusal);

/**
 * Records how the ACTION run's action ended, `failure` NULL when it answered: the run's exit code becomes 0 when it
 * answered and RUNTIME_HOST_EXIT_FAILED when it threw.
 *
 * @return OK; REFUSED for a SERVE run, which runs no action, a run whose started has not answered OK, and a second
 * finished.
 */
spisor_status runtime_host_run_finished(runtime_host_run *run, const char *failure, spisor_refusal *refusal);

/**
 * Ends the run, so no further call acts on it: a run that had not ended fails, its exit code becoming
 * RUNTIME_HOST_EXIT_FAILED, and one that had keeps the exit code it ended with.
 */
void runtime_host_run_fail(runtime_host_run *run);

#endif
