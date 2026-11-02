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
 * Whether reversing a change of this kind must wait for every other unreversed record of the same
 * target, across every registration: a directory and a profile line can be wanted by several hosts.
 */
int runtime_change_is_shared(const char *kind);

/** Whether two records of a shared kind name one target: the same kind and path, and line. */
int runtime_change_same_target(const cJSON *left, const cJSON *right);

/**
 * The record to store for change, without sequence, state or reversed, read from the disk before
 * anything is changed. holder is an unreversed record of the same target another change already
 * holds, or NULL. UNNEEDED when the disk already is as asked and no host holds it, so the thing
 * belongs to the user and nothing is recorded.
 */
runtime_change_step runtime_change_prepare(const cJSON *change, const cJSON *holder, cJSON **record,
                                           runtime_change_note *note);

/** Performs change on the disk; record is the stored record, which may be refined with what was found. */
runtime_change_step runtime_change_perform(const cJSON *change, cJSON *record, runtime_change_note *note);

/** Whether a record left pending by an interrupted apply has its effect on the disk. */
int runtime_change_is_on_disk(const cJSON *record);

/**
 * Undoes record. held is non-zero while another unreversed record holds the same target, and the
 * shared thing is then left. DRIFTED when the target no longer holds what the host made, which is
 * left; note says what.
 *
 * @implNote Reversal is idempotent, so a record whose reversal reached the disk but whose
 * "reversed" mark was not stored is reversed again harmlessly: a json-key already holding its
 * previous value counts as reversed, not drifted.
 */
runtime_change_reversal runtime_change_reverse(const cJSON *record, int held, runtime_change_note *note);

#endif
