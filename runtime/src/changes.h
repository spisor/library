#ifndef RUNTIME_CHANGES_H
#define RUNTIME_CHANGES_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "change_kinds.h"
#include "export.h"

typedef enum runtime_changes_answer {
    RUNTIME_CHANGES_RECORDED,
    RUNTIME_CHANGES_UNNEEDED,
    RUNTIME_CHANGES_REFUSED,
    RUNTIME_CHANGES_FAILED
} runtime_changes_answer;

typedef struct runtime_changes_outcome {
    runtime_changes_answer answer;
    long sequence;
    char reason[1024];
} runtime_changes_outcome;

/**
 * Performs change for host and appends it to the host's "changes" in state/hosts/host.json with the
 * next sequence and "reversed": false. RECORDED carries that sequence. UNNEEDED when the disk already
 * holds what was asked and no host holds it (a directory or profile line the user made), so nothing
 * is changed or recorded; UNNEEDED with the existing sequence when this host already holds an
 * unreversed record of the same change (runtime_change_repeats), which is then answered, not repeated;
 * a file this host already holds that is asked for with new content is rewritten in place under that
 * record and answered the same way. REFUSED and FAILED carry the reason, and then nothing is changed
 * or recorded, with one exception: when a child of a family (a profile-line of a path-entry) cannot be
 * performed, the children already performed are undone, and a child whose undo also failed stays
 * recorded as applied, under its new parent, so the line it left on disk is still reversible.
 * state must be absolute and host must be registered. Paths are stored in the normal form of
 * runtime_change_normalized. The whole apply holds runtime_hosts_lock, so the caller must not hold it.
 * A path-entry on Windows writes the user's Path value under HKCU\Environment, the one change made
 * outside the files a change names (see path_entry_windows.c).
 *
 * @implNote The record is stored with "state": "pending" before the disk is touched and marked
 * "applied" after, so a crash in between leaves a record of a change that may have happened, never a
 * change with no record. Every apply and reverse first settles pending records against the disk:
 * one whose effect is there becomes applied, any other is dropped.
 */
runtime_changes_outcome runtime_changes_apply(const char *state, const char *host, const cJSON *change);

/** runtime_changes_apply for a caller that already holds runtime_hosts_lock, which is not taken again. */
runtime_changes_outcome runtime_changes_apply_held(const char *state, const char *host, const cJSON *change);

/**
 * Reverses entry, one unreversed record in registration of host, held when another unreversed
 * record of its target exists (a registration that cannot be read counts as one), and marks it
 * reversed, and drifted when it drifted, in registration, which the caller stores. The caller holds
 * runtime_hosts_lock. On NOT_REVERSED note says why and nothing is marked; failed is set when memory
 * ran out.
 */
runtime_change_reversal runtime_changes_reverse_entry(const char *state, const char *host, cJSON *registration,
                                                      cJSON *entry, runtime_change_note *note, int *failed);

/**
 * Settles every pending record in registration against the disk, under runtime_hosts_lock held by
 * the caller. Answers 1 when it changed anything.
 */
int runtime_changes_settle_pending(cJSON *registration);

typedef struct runtime_changes_holders {
    cJSON *holder;
    int unreadable;
    int failed;
} runtime_changes_holders;

/**
 * Another unreversed record of record's target, for a shared kind: in registration itself (record
 * excluded) or in any other registration under state/hosts. holder is a copy freed with cJSON_Delete.
 * unreadable is set when some other registration could not be read, failed when memory ran out.
 * The caller holds runtime_hosts_lock, so no registration changes between this check and the act.
 */
runtime_changes_holders runtime_changes_find_holder(const char *state, const char *host, const cJSON *registration,
                                                    const cJSON *record);

typedef int32_t (*runtime_changes_handler)(const char *state, const char *host, const cJSON *argument, uint8_t **out,
                                           size_t *out_len);

/** Parses a changes call, refuses a missing absolute places.state or host, and hands the rest to handler. */
int32_t runtime_changes_serve(const uint8_t *in, size_t in_len, runtime_changes_handler handler, uint8_t **out,
                              size_t *out_len);

/** The JSON function changes.apply: { places: { state }, host, change } answering { recorded, sequence? }. */
RUNTIME_EXPORT int32_t runtime_changes_apply_json(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/**
 * The JSON function changes.reverse: { places: { state }, host } answering { reversed, drifted }.
 * Defined in change_reversal.c.
 */
RUNTIME_EXPORT int32_t runtime_changes_reverse_json(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
