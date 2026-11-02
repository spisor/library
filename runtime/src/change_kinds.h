#ifndef RUNTIME_CHANGE_KINDS_H
#define RUNTIME_CHANGE_KINDS_H

#include "cJSON.h"

typedef enum runtime_change_step {
    RUNTIME_CHANGE_DONE,
    RUNTIME_CHANGE_UNNEEDED,
    RUNTIME_CHANGE_REFUSED,
    RUNTIME_CHANGE_FAILED
} runtime_change_step;

typedef enum runtime_change_reversal {
    RUNTIME_CHANGE_REVERSED,
    RUNTIME_CHANGE_DRIFTED,
    RUNTIME_CHANGE_NOT_REVERSED
} runtime_change_reversal;

/** Why a step refused or failed, or what drifted; static text or a message written into detail. */
typedef struct runtime_change_note {
    char detail[1024];
} runtime_change_note;

/**
 * Why change cannot be recorded, or NULL when it can. Every path a change names must be absolute,
 * so nothing depends on the current directory.
 *
 * @implNote A kind runtime does not know is refused here, before anything is written, because a
 * change that cannot be recorded is never applied (spec section 9).
 */
const char *runtime_change_defect(const cJSON *change);

/**
 * A copy of change, freed with cJSON_Delete, whose path or file is in normal form: joined as Node's
 * path.join does for the host OS (one separator form, "." and ".." resolved, repeats collapsed) and
 * with no trailing separator except on a root. NULL when memory runs out. Call it only on a change
 * runtime_change_defect accepted; the record stores what it answers.
 */
cJSON *runtime_change_normalized(const cJSON *change);

/**
 * Whether two paths name one target: equal in normal form, compared ignoring ASCII case on Windows.
 *
 * @implNote Only ASCII letters are folded, so two Windows spellings differing in the case of a
 * non-ASCII letter still count as two targets. Links are not resolved: two paths reaching one file
 * through a link are two targets.
 */
int runtime_change_paths_equal(const char *left, const char *right);

/**
 * Whether reversing a change of this kind must wait for every other unreversed record of the same
 * target, across every registration: a directory, a profile line and a PATH entry can be wanted by
 * several hosts.
 */
int runtime_change_is_shared(const char *kind);

/**
 * Whether two records of a shared kind name one target: the same kind, path by
 * runtime_change_paths_equal, and line; a path-entry's directory, and on POSIX also its home.
 */
int runtime_change_same_target(const cJSON *left, const cJSON *right);

/**
 * Whether record already holds what change asks for: a file at the same path, a json-key setting the
 * same value at the same key of the same file, or the same target of a shared kind.
 */
int runtime_change_repeats(const cJSON *record, const cJSON *change);

#define RUNTIME_CHANGE_DIGEST_SIZE 32

/**
 * Whether change asks the file record holds for content other than it was last written with, by the
 * "digest" the record keeps; a record with no digest counts as asking.
 */
int runtime_change_asks_new_content(const cJSON *record, const cJSON *change);

/**
 * Writes change's content over the host's own file at record's path, creating it again when it is
 * gone, and stores the new digest in record. REFUSED, with the disk unchanged, when a directory or a
 * link now stands there.
 *
 * @implNote A link is refused rather than followed, so rewriting a shim can never write through to
 * the program the link points at.
 */
runtime_change_step runtime_change_rewrite(const cJSON *change, cJSON *record, runtime_change_note *note);

/**
 * The changes a change is applied as, recorded after it as its children, each an entry of its own with
 * "parent" set to the change's sequence: an array freed with cJSON_Delete, or NULL for a kind that has
 * none. A POSIX path-entry is one profile-line per profile that exists, so each line keeps its own
 * reference count and reversal.
 */
runtime_change_step runtime_change_children(const cJSON *change, cJSON **children, runtime_change_note *note);

/**
 * The record to store for change, without sequence, state or reversed, read from the disk before
 * anything is changed. holder is an unreversed record of the same target another change already
 * holds, or NULL. UNNEEDED when the disk already is as asked and no host holds it, so the thing
 * belongs to the user and nothing is recorded; a json-key already holding its value is UNNEEDED too.
 */
runtime_change_step runtime_change_prepare(const cJSON *change, const cJSON *holder, cJSON **record,
                                           runtime_change_note *note);

/** Performs change on the disk; record is the stored record, which may be refined with what was found. */
runtime_change_step runtime_change_perform(const cJSON *change, cJSON *record, runtime_change_note *note);

/** Whether a record left pending by an interrupted apply has its effect on the disk. */
int runtime_change_is_on_disk(const cJSON *record);

/**
 * Undoes record. held is non-zero while another unreversed record holds the same target, and the
 * shared thing is then left. DRIFTED when the target no longer holds what the host made, or a
 * directory the host created still holds something, which is left; note says what.
 *
 * @implNote Reversal is idempotent, so a record whose reversal reached the disk but whose
 * "reversed" mark was not stored is reversed again harmlessly: a json-key already holding its
 * previous value counts as reversed, not drifted.
 */
runtime_change_reversal runtime_change_reverse(const cJSON *record, int held, runtime_change_note *note);

#endif
