#ifndef RUNTIME_CHANGES_H
#define RUNTIME_CHANGES_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "change_kinds.h"
#include "registry_snapshot.h"

typedef enum runtime_changes_answer {
    RUNTIME_CHANGES_RECORDED,
    RUNTIME_CHANGES_UNNEEDED,
    RUNTIME_CHANGES_REFUSED,
    RUNTIME_CHANGES_FAILED,
    RUNTIME_CHANGES_OUT_OF_MEMORY
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
 * or recorded; OUT_OF_MEMORY changes nothing either, though once the pending record was stored the
 * number it took is not handed out again; with one exception: when a child of a family (a profile-line
 * of a path-entry) cannot be performed, the children already performed are undone, and a child whose
 * undo also failed stays recorded as applied, under its new parent, so the line it left on disk is
 * still reversible.
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
 * Reverses, newest first, every unreversed change of host among the count sequences listed, as the reverse request
 * does, for a caller that already holds runtime_hosts_lock. Answers 1 once none is left, 0 when one could not be
 * reversed or memory ran out, the rest then left recorded; *out_of_memory set when memory ran out.
 */
int runtime_changes_reverse_listed_held(const char *state, const char *host, const double *sequences, size_t count,
                                        int *out_of_memory);

typedef struct runtime_changes_kept runtime_changes_kept;

/**
 * What takes back every change one call records while it holds runtime_hosts_lock from its first apply to its last:
 * the host's registration as the call found it, and each change's undo, prepared before the change was performed, so
 * a call that fails is taken back without allocating (prepared_undo.h). A zeroed journal is an empty one.
 */
typedef struct runtime_changes_journal {
    runtime_registry_snapshot registration;
    runtime_changes_kept *kept;
    double *sequences;
    size_t count;
    size_t capacity;
    int begun;
    int whole;
} runtime_changes_journal;

/** runtime_changes_apply_held, keeping in journal the undo of each change it records. */
runtime_changes_outcome runtime_changes_apply_journaled(const char *state, const char *host, const cJSON *change,
                                                        runtime_changes_journal *journal);

/**
 * Undoes, newest first, every change journal kept, and writes the registration back as the call found it, allocating
 * nothing, under runtime_hosts_lock still held since the call's first apply: 1 when the call left nothing.
 *
 * @implNote The sequences the undone changes took are handed out again, since no answer ever named them. It stops at
 * an undo that fails, and writes nothing back when one of the call's changes stayed on the disk unkept, since that
 * would drop the record of what stayed.
 */
int runtime_changes_undo_journal(const runtime_changes_journal *journal);

/**
 * runtime_changes_undo_journal, and when that leaves something, every change journal kept reversed through its
 * record, which allocates: 1 when the call left nothing; *out_of_memory set when memory ran out, the rest then left
 * recorded. Defined in change_reversal.c.
 */
int runtime_changes_take_back(const char *state, const char *host, const runtime_changes_journal *journal,
                              int *out_of_memory);

/** Releases what journal holds, the changes it kept staying as they are; it is empty again afterwards. */
void runtime_changes_end_journal(runtime_changes_journal *journal);

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
 * the caller. Answers 1 when it changed anything; when memory runs out it sets *out_of_memory and
 * answers 0, the registration then to be discarded rather than stored.
 */
int runtime_changes_settle_pending(cJSON *registration, int *out_of_memory);

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

/**
 * The {message, fix} refusal a REFUSED or FAILED outcome of runtime_changes_apply answers; NULL for any other outcome,
 * or when memory runs out.
 */
cJSON *runtime_changes_refusal(const runtime_changes_outcome *outcome);

/**
 * Reverses, newest first, every unreversed change `host` recorded in `state`: { reversed, drifted }, or NULL with
 * *refusal set, or with it NULL when memory ran out. Defined in change_reversal.c.
 */
cJSON *runtime_changes_reverse(const char *state, const char *host, cJSON **refusal);

#endif
