#include <stdio.h>
#include <string.h>

#include "change_kinds.h"
#include "changes.h"
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

static int mark_reversed(cJSON *entry, int drifted) {
    cJSON *flag = cJSON_CreateTrue();
    int marked = flag != NULL && (cJSON_GetObjectItemCaseSensitive(entry, "reversed") != NULL
                                      ? cJSON_ReplaceItemInObjectCaseSensitive(entry, "reversed", flag)
                                      : cJSON_AddItemToObject(entry, "reversed", flag));
    if (flag != NULL && !marked) {
        cJSON_Delete(flag);
    }
    return marked && (!drifted || cJSON_AddTrueToObject(entry, "drifted") != NULL);
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

static int is_held(const reversal_session *session, const cJSON *entry, int *failed) {
    runtime_changes_holders holders =
        runtime_changes_find_holder(session->state, session->host, session->registration, entry);
    int held = holders.holder != NULL || holders.unreadable;
    *failed = holders.failed;
    cJSON_Delete(holders.holder);
    return held;
}

/** Reverses one change and stores its mark; answers SUCCESS to go on, anything else to stop with *out filled. */
static int32_t reverse_one(reversal_session *session, cJSON *entry, uint8_t **out, size_t *out_len) {
    int failed = 0;
    int held = is_held(session, entry, &failed);
    if (failed) {
        return out_of_memory(out, out_len);
    }
    runtime_change_note note = {""};
    runtime_change_reversal reversal = runtime_change_reverse(entry, held, &note);
    if (reversal == RUNTIME_CHANGE_NOT_REVERSED) {
        return refuse_stuck(entry, note.detail, out, out_len);
    }
    int drifted = reversal == RUNTIME_CHANGE_DRIFTED;
    if ((drifted && !add_drift(session->drifted, entry, note.detail)) || !mark_reversed(entry, drifted)) {
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

static int32_t reverse_all(reversal_session *session, cJSON *result, uint8_t **out, size_t *out_len) {
    if (runtime_changes_settle_pending(session->registration) &&
        runtime_registry_store(session->state, session->registration) != RUNTIME_REGISTRY_OK) {
        return refuse_unwritable(session, out, out_len);
    }
    cJSON *entry = newest_unreversed(session->registration);
    while (entry != NULL && may_reverse_another()) {
        int32_t status = reverse_one(session, entry, out, out_len);
        if (status != SPISOR_STATUS_SUCCESS) {
            return status;
        }
        entry = newest_unreversed(session->registration);
    }
    return answer_reversed(session, result, out, out_len);
}

static int32_t refuse_unreadable(const char *host, uint8_t **out, size_t *out_len) {
    char message[512];
    snprintf(message, sizeof message, "the registration of host %s is not readable", host);
    return spisor_fail(message, "repair or restore it; nothing was reversed", out, out_len);
}

static int32_t reverse_host(const char *state, const char *host, const cJSON *argument, uint8_t **out,
                            size_t *out_len) {
    (void)argument;
    reversal_session session = {state, host, NULL, NULL, 0};
    runtime_registry_answer loaded = runtime_registry_load(state, host, &session.registration);
    if (loaded == RUNTIME_REGISTRY_INVALID) {
        return spisor_fail("the host id is not a portable path segment", "send the id the host registered with",
                           out, out_len);
    }
    if (loaded == RUNTIME_REGISTRY_UNREADABLE) {
        return refuse_unreadable(host, out, out_len);
    }
    if (loaded == RUNTIME_REGISTRY_OUT_OF_MEMORY) {
        return out_of_memory(out, out_len);
    }
    cJSON *result = cJSON_CreateObject();
    session.drifted = result == NULL ? NULL : cJSON_AddArrayToObject(result, "drifted");
    int32_t status =
        session.drifted == NULL ? out_of_memory(out, out_len) : reverse_all(&session, result, out, out_len);
    cJSON_Delete(result);
    cJSON_Delete(session.registration);
    return status;
}

RUNTIME_EXPORT int32_t runtime_changes_reverse_json(const uint8_t *in, size_t in_len, uint8_t **out,
                                                    size_t *out_len) {
    return runtime_changes_serve(in, in_len, reverse_host, out, out_len);
}
