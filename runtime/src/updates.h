#ifndef RUNTIME_UPDATES_H
#define RUNTIME_UPDATES_H

#include "cJSON.h"
#include "updates_request.h"

/**
 * @file
 * The updates service's operations over JSON, for runtime's own callers: each answers its result, or NULL with
 * *refusal set to {message, fix}, or with it NULL when memory runs out.
 */

/**
 * updates' apply: {places, hostId, settings?, defaults?, running?, libraries?, platform?}, what a start of
 * hostId does before it runs anything: switches hostId to the version its references schedule once that
 * is installed intact, keeping the one switched from as rollback until a later start; resolves the
 * plugins the settings and defaults choose, from installed versions only; records run, next and rollback
 * in state/bundles/<hostId>.json and this start's own run record; and answers {plugins, run, next,
 * rollback, unrecorded, runRecord}. A plugin whose version changed keeps the one it ran before as its
 * rollback too, and a plugin a rollback moved away from a version keeps the version rolled back to while
 * that one is still what resolves. unrecorded lists, as {message, fix}, what the start could not record
 * without that stopping it, such as a boot hint that could not be written. running names the other versions the start runs beside its plugins, such as the
 * libraries it loaded. With libraries, the ids of the libraries the host boots with, it also resolves
 * those for the next start, schedules a changed one, and records them in hostId's boot hint with the
 * host version it runs.
 *
 * @implNote The references are read, changed and written under the registry lock (cache.sweep holds the
 * same one), so a concurrent check's next is never lost. A fetched version switches on the next start,
 * never this one (spec 7.2).
 */
cJSON *runtime_updates_apply_operation(const cJSON *argument, cJSON **refusal);

/**
 * updates' check: {places, hostId, now, settings?, defaults?, problems?, platform?} checks every catalog
 * source the settings name, at most once per updates.intervalHours (24 by default) for an https one;
 * schedules as next in hostId's references every damaged version problems names, every newer version of a
 * plugin the settings choose, and the version hostId and spisor-host are held to; installs each; switches
 * the boot hint of a host it switches to once that is installed; sweeps the cache; and answers {checked,
 * fetched, refused, removed}.
 *
 * A version a rollback moved away from is not scheduled again until the catalog's latest is another one.
 * A boot hint that could not be switched is reported in refused beside the version's fetched entry.
 *
 * @implNote next is recorded under the registry lock before anything is fetched, so no sweep takes a
 * version still arriving, and the downloads themselves hold no lock, so no start or registration waits
 * on one; what runs now is never changed. The timer that calls this stays in the language layer.
 */
cJSON *runtime_updates_check_operation(const cJSON *argument, cJSON **refusal);

/**
 * updates' rollback: {places, hostId, item} switches item back to the version hostId's last switch kept
 * as rollback, keeping the one switched away from as rollback in turn and as the version no check
 * schedules again until the catalog moves past it, and makes it the version item's boot hint names;
 * answers {run, next, rollback}. item may be a host or a plugin. Refused when no rollback is kept for item, or that
 * version is no longer installed intact.
 */
cJSON *runtime_updates_rollback_operation(const cJSON *argument, cJSON **refusal);

/**
 * As runtime_updates_rollback_operation, calling `fill` with the answer and `context` before anything is written, so
 * a consumer copying the answer runs out of memory before the rollback rather than after it.
 */
cJSON *runtime_updates_rollback_filling(const cJSON *argument, runtime_updates_precommit fill, void *context,
                                       cJSON **refusal);

/**
 * updates' recorded: {places, hostId} answers {run}, what hostId's last start recorded running, an empty
 * run when no references file exists; a file that exists and cannot be read is refused, naming it. It
 * switches, schedules and records nothing, so a second process of a running host can start what that
 * host runs without moving it to another version.
 */
cJSON *runtime_updates_recorded_operation(const cJSON *argument, cJSON **refusal);

/**
 * updates' run: {places, now} runs updates.check for every registered host, spisor-host first with the
 * catalog sources its settings record lent to every other check, and install.application with operation
 * update for each loader whose application an install method installed; answers {checks, applications,
 * refused}, each refusal carrying the id it was for.
 */
cJSON *runtime_updates_run_operation(const cJSON *argument, cJSON **refusal);

#endif
