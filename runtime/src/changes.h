#ifndef RUNTIME_CHANGES_H
#define RUNTIME_CHANGES_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
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
 * is changed or recorded. REFUSED and FAILED carry the reason, and then nothing is changed or recorded.
 * state must be absolute and host must be registered. Paths are stored in the normal form of
 * runtime_change_normalized. The whole apply holds runtime_hosts_lock, so the caller must not hold it.
 *
 * @implNote The record is stored with "state": "pending" before the disk is touched and marked
 * "applied" after, so a crash in between leaves a record of a change that may have happened, never a
 * change with no record. Every apply and reverse first settles pending records against the disk:
 * one whose effect is there becomes applied, any other is dropped.
 */
runtime_changes_outcome runtime_changes_apply(const char *state, const char *host, const cJSON *change);

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
