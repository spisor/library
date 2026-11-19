#ifndef RUNTIME_UPDATES_H
#define RUNTIME_UPDATES_H

#include "cJSON.h"
#include "updates_target.h"

/**
 * @file
 * The updates service's functions, for its definition and runtime's own callers: each answers its result, or NULL
 * with *refusal set to {message, fix}, or with it NULL when memory runs out. A target's state and cache are absolute
 * and its host id plain; an apply or a check for a target with no platform key is refused.
 */

/**
 * What a start applies: settings, the host's settings values or NULL; defaults, an object of binding to plugin or
 * NULL; running, an object of the ids the start runs beside its plugins to their versions, or NULL; libraries, an
 * array of the ids of the libraries the host boots with, or NULL when the start writes no boot hint.
 */
typedef struct runtime_updates_apply_request {
    runtime_updates_target target;
    const cJSON *settings;
    const cJSON *defaults;
    const cJSON *running;
    const cJSON *libraries;
} runtime_updates_apply_request;

/**
 * updates' apply, what a start of the target's host does before it runs anything: switches hostId to the version its
 * references schedule once that is installed intact, keeping the one switched from as rollback until a later start;
 * resolves the plugins the settings and defaults choose, from installed versions only; records run, next and rollback
 * in state/bundles/<hostId>.json and this start's own run record; and answers {plugins, run, next, rollback,
 * unrecorded, runRecord}. A plugin whose version changed keeps the one it ran before as its rollback too, and a plugin
 * a rollback moved away from a version keeps the version rolled back to while that one is still what resolves.
 * unrecorded lists, as {message, fix}, what the start could not record without that stopping it, such as a boot hint
 * that could not be written. running names the other versions the start runs beside its plugins, such as the libraries
 * it loaded. With libraries, the ids of the libraries the host boots with, it also resolves those for the next start,
 * schedules a changed one, and records them in hostId's boot hint with the host version it runs.
 *
 * @implNote The references are read, changed and written under the registry lock (cache.sweep holds the
 * same one), so a concurrent check's next is never lost. A fetched version switches on the next start,
 * never this one (spec 7.2).
 */
cJSON *runtime_updates_apply(const runtime_updates_apply_request *request, cJSON **refusal);

/**
 * What a check checks at now, the time of the check as toISOString writes it: settings and defaults as an apply
 * takes them, and problems, an array of the {id, version} found damaged, or NULL.
 */
typedef struct runtime_updates_check_request {
    runtime_updates_target target;
    const char *now;
    const cJSON *settings;
    const cJSON *defaults;
    const cJSON *problems;
} runtime_updates_check_request;

/**
 * updates' check: checks every catalog
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
cJSON *runtime_updates_check(const runtime_updates_check_request *request, cJSON **refusal);

/**
 * updates' rollback: switches item back to the version the target host's last switch kept as rollback, keeping the
 * one switched away from as rollback in turn and as the version no check schedules again until the catalog moves past
 * it, and makes it the version item's boot hint names; answers {run, next, rollback}. item may be a host or a
 * plugin. Refused when no rollback is kept for item, or that version is no longer installed intact. A `fill` that is
 * not NULL is called with the answer and `context` before anything is written, so a consumer copying the answer runs
 * out of memory before the rollback rather than after it.
 */
cJSON *runtime_updates_rollback(const runtime_updates_target *target, const char *item,
                                runtime_updates_precommit fill, void *context, cJSON **refusal);

/**
 * updates' recorded: answers {run}, what the target host's last start recorded running, an empty
 * run when no references file exists; a file that exists and cannot be read is refused, naming it. It
 * switches, schedules and records nothing, so a second process of a running host can start what that
 * host runs without moving it to another version.
 */
cJSON *runtime_updates_recorded(const runtime_updates_target *target, cJSON **refusal);

/**
 * updates' run: runs updates.check at now for every registered host under the absolute places, spisor-host first
 * with the catalog sources its settings record lent to every other check, and install.application with operation
 * update for each loader whose application an install method installed; answers {checks, applications, refused},
 * each refusal carrying the id it was for.
 */
cJSON *runtime_updates_run(const char *portable, const char *state, const char *cache, const char *now,
                          cJSON **refusal);

#endif
