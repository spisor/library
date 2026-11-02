#include <stdio.h>
#include <string.h>

#include "change_kinds.h"
#include "changes.h"
#include "hosts_lock.h"
#include "registry.h"
#include "spisor/json_kind.h"

#ifdef SPISOR_CHANGES_TEST_HOOKS
#include "change_reversal_hooks.h"

static long reversals_allowed = -1;

void runtime_changes_allow_reversals(long count) {
    reversals_allowed = count;
}

static int may_reverse_another(void) {
    if (reversals_allowed < 0) {
        return 1;
    }
    if (reversals_allowed == 0) {
        return 0;
    }
    reversals_allowed--;
    return 1;
}
#else
static int may_reverse_another(void) {
    return 1;
}
#endif

typedef struct reversal_session {
    const char *state;
    const char *host;
    cJSON *registration;
    cJSON *drifted;
    int reversed;
    int finished;
} reversal_session;

static int32_t out_of_memory(uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    return SPISOR_STATUS_OUT_OF_MEMORY;
}

static double sequence_of(const cJSON *entry) {
    const cJSON *sequence = cJSON_GetObjectItemCaseSensitive(entry, "sequence");
    return cJSON_IsNumber(sequence) ? sequence->valuedouble : 0;
}

static cJSON *newest_unreversed(const cJSON *registration) {
    cJSON *newest = NULL;
    cJSON *entry;
    cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(registration, "changes")) {
        int unreversed = !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "reversed"));
        if (unreversed && (newest == NULL || sequence_of(entry) > sequence_of(newest))) {
            newest = entry;
        }
    }
    return newest;
}

static int add_drift(cJSON *drifted, const cJSON *entry, const char *detail) {
    cJSON *report = cJSON_CreateObject();
    int complete = report != NULL && cJSON_AddNumberToObject(report, "sequence", sequence_of(entry)) != NULL &&
                   cJSON_AddStringToObject(report, "kind",
                                           cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "kind"))) !=
                       NULL &&
                   cJSON_AddStringToObject(report, "detail", detail) != NULL;
    if (complete && cJSON_AddItemToArray(drifted, report)) {
        return 1;
    }
    cJSON_Delete(report);
    return 0;
}

static int32_t refuse_unwritable(const reversal_session *session, uint8_t **out, size_t *out_len) {
    char message[1024];
    snprintf(message, sizeof message, "the registration of host %s could not be written under %s", session->host,
             session->state);
    return spisor_fail(message, "make the registry writable and reverse again; what was reversed stays marked", out,
                       out_len);
}

static int32_t refuse_stuck(const cJSON *entry, const char *detail, uint8_t **out, size_t *out_len) {
    char message[1400];
    snprintf(message, sizeof message, "change %.0f could not be reversed: %s", sequence_of(entry), detail);
    return spisor_fail(message, "fix the cause and reverse again; the newer changes stay reversed and none is "
                                "reversed twice",
                       out, out_len);
}

/** Reverses one change and stores its mark; answers SUCCESS to go on, anything else to stop with *out filled. */
static int32_t reverse_one(reversal_session *session, cJSON *entry, uint8_t **out, size_t *out_len) {
    int failed = 0;
    runtime_change_note note = {""};
    runtime_change_reversal reversal =
        runtime_changes_reverse_entry(session->state, session->host, session->registration, entry, &note, &failed);
    if (failed) {
        return out_of_memory(out, out_len);
    }
    if (reversal == RUNTIME_CHANGE_NOT_REVERSED) {
        return refuse_stuck(entry, note.detail, out, out_len);
    }
    if (reversal == RUNTIME_CHANGE_DRIFTED && !add_drift(session->drifted, entry, note.detail)) {
        return out_of_memory(out, out_len);
    }
    if (runtime_registry_store(session->state, session->registration) != RUNTIME_REGISTRY_OK) {
        return refuse_unwritable(session, out, out_len);
    }
    session->reversed++;
    return SPISOR_STATUS_SUCCESS;
}

static int32_t answer_reversed(const reversal_session *session, cJSON *result, uint8_t **out, size_t *out_len) {
    if (cJSON_AddNumberToObject(result, "reversed", session->reversed) == NULL) {
        return out_of_memory(out, out_len);
    }
    return spisor_succeed(result, out, out_len);
}

static int32_t refuse_unreadable(const char *host, uint8_t **out, size_t *out_len) {
    char message[512];
    snprintf(message, sizeof message, "the registration of host %s is not readable", host);
    return spisor_fail(message, "repair or restore it; nothing more was reversed", out, out_len);
}

static int32_t refuse_load(runtime_registry_answer loaded, const char *host, uint8_t **out, size_t *out_len) {
    if (loaded == RUNTIME_REGISTRY_INVALID) {
        return spisor_fail("the host id is not a portable path segment", "send the id the host registered with",
                           out, out_len);
    }
    if (loaded == RUNTIME_REGISTRY_UNREADABLE) {
        return refuse_unreadable(host, out, out_len);
    }
    return out_of_memory(out, out_len);
}

static int32_t finish(reversal_session *session, int settled, uint8_t **out, size_t *out_len) {
    session->finished = 1;
    if (settled && runtime_registry_store(session->state, session->registration) != RUNTIME_REGISTRY_OK) {
        return refuse_unwritable(session, out, out_len);
    }
    return SPISOR_STATUS_SUCCESS;
}

/** One step under the lock: read the registration afresh, settle it, reverse its newest change and store. */
static int32_t reverse_step(reversal_session *session, uint8_t **out, size_t *out_len) {
    runtime_registry_answer loaded = runtime_registry_load(session->state, session->host, &session->registration);
    if (loaded == RUNTIME_REGISTRY_ABSENT) {
        session->finished = 1;
        return SPISOR_STATUS_SUCCESS;
    }
    if (loaded != RUNTIME_REGISTRY_OK) {
        return refuse_load(loaded, session->host, out, out_len);
    }
    int settled = runtime_changes_settle_pending(session->registration);
    cJSON *entry = newest_unreversed(session->registration);
    if (entry == NULL || !may_reverse_another()) {
        return finish(session, settled, out, out_len);
    }
    return reverse_one(session, entry, out, out_len);
}

static int32_t refuse_unlocked(const char *state, uint8_t **out, size_t *out_len) {
    char fix[2048];
    snprintf(fix, sizeof fix, "check that %s/hosts/.lock can be created, opened and locked", state);
    return spisor_fail("the registry lock could not be taken", fix, out, out_len);
}

/**
 * @implNote The lock is held per change, not across the removal, so another host can apply or
 * reverse between two steps; each step reads every registration afresh under the lock, so its
 * holder check, its act on the disk and its store are one atomic step.
 */
static int32_t reverse_all(reversal_session *session, cJSON *result, uint8_t **out, size_t *out_len) {
    int32_t status = SPISOR_STATUS_SUCCESS;
    while (status == SPISOR_STATUS_SUCCESS && !session->finished) {
        runtime_hosts_guard *guard = runtime_hosts_lock(session->state);
        if (guard == NULL) {
            return refuse_unlocked(session->state, out, out_len);
        }
        status = reverse_step(session, out, out_len);
        runtime_hosts_unlock(guard);
        cJSON_Delete(session->registration);
        session->registration = NULL;
    }
    return status == SPISOR_STATUS_SUCCESS ? answer_reversed(session, result, out, out_len) : status;
}

static int32_t reverse_host(const char *state, const char *host, const cJSON *argument, uint8_t **out,
                            size_t *out_len) {
    (void)argument;
    reversal_session session = {state, host, NULL, NULL, 0, 0};
    cJSON *result = cJSON_CreateObject();
    session.drifted = result == NULL ? NULL : cJSON_AddArrayToObject(result, "drifted");
    int32_t status =
        session.drifted == NULL ? out_of_memory(out, out_len) : reverse_all(&session, result, out, out_len);
    cJSON_Delete(result);
    return status;
}

RUNTIME_EXPORT int32_t runtime_changes_reverse_json(const uint8_t *in, size_t in_len, uint8_t **out,
                                                    size_t *out_len) {
    return runtime_changes_serve(in, in_len, reverse_host, out, out_len);
}
