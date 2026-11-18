#include <stdio.h>
#include <string.h>

#include "change_kinds.h"
#include "changes.h"
#include "hosts_lock.h"
#include "json_service.h"
#include "registry.h"

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
    double above;
    const double *listed;
    size_t listed_count;
    int lock_held;
    int reversed;
    int finished;
} reversal_session;

/** A step's stop for memory running out: 0, with no refusal. */
static int out_of_memory(cJSON **refusal) {
    *refusal = NULL;
    return 0;
}

/** A step's stop with a refusal, which is NULL when memory ran out building it: 0. */
static int fail(const char *message, const char *fix, cJSON **refusal) {
    *refusal = runtime_refusal(message, fix);
    return 0;
}

static double sequence_of(const cJSON *entry) {
    const cJSON *sequence = cJSON_GetObjectItemCaseSensitive(entry, "sequence");
    return cJSON_IsNumber(sequence) ? sequence->valuedouble : 0;
}

/** Whether the session may reverse entry: one of its listed sequences when it lists any, else any above `above`. */
static int is_selected(const reversal_session *session, const cJSON *entry) {
    double sequence = sequence_of(entry);
    if (session->listed == NULL) {
        return sequence > session->above;
    }
    for (size_t index = 0; index < session->listed_count; index++) {
        if (session->listed[index] == sequence) {
            return 1;
        }
    }
    return 0;
}

static cJSON *newest_unreversed(const reversal_session *session) {
    cJSON *newest = NULL;
    cJSON *entry;
    cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(session->registration, "changes")) {
        int unreversed = !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "reversed"));
        if (unreversed && is_selected(session, entry) &&
            (newest == NULL || sequence_of(entry) > sequence_of(newest))) {
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

static int refuse_unwritable(const reversal_session *session, cJSON **refusal) {
    char message[1024];
    snprintf(message, sizeof message, "the registration of host %s could not be written under %s", session->host,
             session->state);
    return fail(message, "make the registry writable and reverse again; what was reversed stays marked", refusal);
}

static int refuse_stuck(const cJSON *entry, const char *detail, cJSON **refusal) {
    char message[1400];
    snprintf(message, sizeof message, "change %.0f could not be reversed: %s", sequence_of(entry), detail);
    return fail(message, "fix the cause and reverse again; the newer changes stay reversed and none is reversed twice",
                refusal);
}

/** Reverses one change and stores its mark; answers 1 to go on, 0 to stop with *refusal set or not. */
static int reverse_one(reversal_session *session, cJSON *entry, cJSON **refusal) {
    int failed = 0;
    runtime_change_note note = {""};
    runtime_change_reversal reversal =
        runtime_changes_reverse_entry(session->state, session->host, session->registration, entry, &note, &failed);
    if (failed) {
        return out_of_memory(refusal);
    }
    if (reversal == RUNTIME_CHANGE_NOT_REVERSED) {
        return refuse_stuck(entry, note.detail, refusal);
    }
    if (reversal == RUNTIME_CHANGE_DRIFTED && !add_drift(session->drifted, entry, note.detail)) {
        return out_of_memory(refusal);
    }
    runtime_registry_answer stored = runtime_registry_store(session->state, session->registration);
    if (stored != RUNTIME_REGISTRY_OK) {
        return stored == RUNTIME_REGISTRY_OUT_OF_MEMORY ? out_of_memory(refusal) : refuse_unwritable(session, refusal);
    }
    session->reversed++;
    return 1;
}

static int refuse_unreadable(const char *host, cJSON **refusal) {
    char message[512];
    snprintf(message, sizeof message, "the registration of host %s is not readable", host);
    return fail(message, "repair or restore it; nothing more was reversed", refusal);
}

static int refuse_load(runtime_registry_answer loaded, const char *host, cJSON **refusal) {
    if (loaded == RUNTIME_REGISTRY_INVALID) {
        return fail("the host id is not a portable path segment", "send the id the host registered with", refusal);
    }
    if (loaded == RUNTIME_REGISTRY_UNREADABLE) {
        return refuse_unreadable(host, refusal);
    }
    return out_of_memory(refusal);
}

static int finish(reversal_session *session, int settled, cJSON **refusal) {
    session->finished = 1;
    runtime_registry_answer stored =
        settled ? runtime_registry_store(session->state, session->registration) : RUNTIME_REGISTRY_OK;
    if (stored != RUNTIME_REGISTRY_OK) {
        return stored == RUNTIME_REGISTRY_OUT_OF_MEMORY ? out_of_memory(refusal) : refuse_unwritable(session, refusal);
    }
    return 1;
}

/** One step under the lock: read the registration afresh, settle it, reverse its newest change and store. */
static int reverse_step(reversal_session *session, cJSON **refusal) {
    int out_of_memory_settling = 0;
    runtime_registry_answer loaded = runtime_registry_load(session->state, session->host, &session->registration);
    if (loaded == RUNTIME_REGISTRY_ABSENT) {
        session->finished = 1;
        return 1;
    }
    if (loaded != RUNTIME_REGISTRY_OK) {
        return refuse_load(loaded, session->host, refusal);
    }
    int settled = runtime_changes_settle_pending(session->registration, &out_of_memory_settling);
    if (out_of_memory_settling) {
        return out_of_memory(refusal);
    }
    cJSON *entry = newest_unreversed(session);
    if (entry == NULL || !may_reverse_another()) {
        return finish(session, settled, refusal);
    }
    return reverse_one(session, entry, refusal);
}

static int refuse_unlocked(const char *state, cJSON **refusal) {
    char fix[2048];
    snprintf(fix, sizeof fix, "check that %s/hosts/.lock can be created, opened and locked", state);
    return fail("the registry lock could not be taken", fix, refusal);
}

/**
 * @implNote Unless the caller holds it, the lock is taken per change, not across the removal, so another host can
 * apply or reverse between two steps; each step reads every registration afresh under the lock, so its holder check,
 * its act on the disk and its store are one atomic step.
 */
static int reverse_steps(reversal_session *session, cJSON **refusal) {
    int going_on = 1;
    while (going_on && !session->finished) {
        spisor_lock *guard = NULL;
        spisor_status locked = session->lock_held ? SPISOR_OK : runtime_hosts_lock(session->state, &guard);
        if (locked == SPISOR_OUT_OF_MEMORY) {
            return out_of_memory(refusal);
        }
        if (locked != SPISOR_OK) {
            return refuse_unlocked(session->state, refusal);
        }
        going_on = reverse_step(session, refusal);
        spisor_unlock(guard);
        cJSON_Delete(session->registration);
        session->registration = NULL;
    }
    return going_on;
}

static int reverse_all(reversal_session *session, cJSON *result, cJSON **refusal) {
    if (!reverse_steps(session, refusal)) {
        return 0;
    }
    return cJSON_AddNumberToObject(result, "reversed", session->reversed) != NULL || out_of_memory(refusal);
}

/** Reverses as the session selects, under the caller's lock; *out_of_memory set when memory stopped it. */
static int reverse_held(reversal_session *session, int *out_of_memory) {
    cJSON *refusal = NULL;
    session->drifted = cJSON_CreateArray();
    int reversed = session->drifted != NULL && reverse_steps(session, &refusal);
    *out_of_memory = !reversed && refusal == NULL;
    cJSON_Delete(refusal);
    cJSON_Delete(session->drifted);
    return reversed;
}

int runtime_changes_reverse_newer_held(const char *state, const char *host, double above) {
    int out_of_memory = 0;
    reversal_session session = {state, host, NULL, NULL, above, NULL, 0, 1, 0, 0};
    return reverse_held(&session, &out_of_memory);
}

int runtime_changes_reverse_listed_held(const char *state, const char *host, const double *sequences, size_t count,
                                        int *out_of_memory) {
    reversal_session session = {state, host, NULL, NULL, 0, sequences, count, 1, 0, 0};
    return reverse_held(&session, out_of_memory);
}

cJSON *runtime_changes_reverse(const char *state, const char *host, cJSON **refusal) {
    reversal_session session = {state, host, NULL, NULL, -1, NULL, 0, 0, 0, 0};
    cJSON *result = cJSON_CreateObject();
    *refusal = NULL;
    session.drifted = result == NULL ? NULL : cJSON_AddArrayToObject(result, "drifted");
    if (session.drifted == NULL || !reverse_all(&session, result, refusal)) {
        cJSON_Delete(result);
        return NULL;
    }
    return result;
}
