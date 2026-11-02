#include "changes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "change_kinds.h"
#include "directory_tree.h"
#include "hosts_lock.h"
#include "paths.h"
#include "registry.h"
#include "spisor/json_kind.h"
#include "spisor/path.h"

#define REGISTRATION_SUFFIX ".json"
#define REGISTRATION_SUFFIX_LENGTH 5

typedef struct prepared_child {
    const cJSON *change;
    cJSON *record;
    cJSON *entry;
    int held;
    int undo_failed;
} prepared_child;

typedef struct prepared_children {
    prepared_child *items;
    size_t count;
} prepared_children;

typedef struct apply_session {
    const char *state;
    const char *host;
    const cJSON *change;
    cJSON *registration;
    runtime_changes_outcome outcome;
} apply_session;

static const char *text_of(const cJSON *object, const char *member) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, member));
}

static cJSON *changes_of(const cJSON *registration) {
    return cJSON_GetObjectItemCaseSensitive(registration, "changes");
}

static int is_unreversed(const cJSON *entry) {
    return !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "reversed"));
}

static int is_pending(const cJSON *entry) {
    const char *state = text_of(entry, "state");
    return state != NULL && strcmp(state, "pending") == 0;
}

static int set_text(cJSON *object, const char *member, const char *text) {
    cJSON *value = cJSON_CreateString(text);
    int set = value != NULL && cJSON_ReplaceItemInObjectCaseSensitive(object, member, value);
    if (value != NULL && !set) {
        cJSON_Delete(value);
    }
    return set;
}

int runtime_changes_settle_pending(cJSON *registration) {
    cJSON *changes = changes_of(registration);
    int changed = 0;
    cJSON *entry = changes == NULL ? NULL : changes->child;
    while (entry != NULL) {
        cJSON *next = entry->next;
        if (is_pending(entry)) {
            if (runtime_change_is_on_disk(entry)) {
                set_text(entry, "state", "applied");
            } else {
                cJSON_Delete(cJSON_DetachItemViaPointer(changes, entry));
            }
            changed = 1;
        }
        entry = next;
    }
    return changed;
}

static const cJSON *holder_in(const cJSON *registration, const cJSON *record) {
    const cJSON *entry;
    cJSON_ArrayForEach(entry, changes_of(registration)) {
        if (entry != record && is_unreversed(entry) && runtime_change_same_target(entry, record)) {
            return entry;
        }
    }
    return NULL;
}

static int is_registration_of_another(const runtime_directory_entry *entry, const char *host) {
    size_t length = strlen(entry->name);
    return !entry->is_directory && length > REGISTRATION_SUFFIX_LENGTH &&
           strcmp(entry->name + length - REGISTRATION_SUFFIX_LENGTH, REGISTRATION_SUFFIX) == 0 &&
           !(length - REGISTRATION_SUFFIX_LENGTH == strlen(host) &&
             strncmp(entry->name, host, length - REGISTRATION_SUFFIX_LENGTH) == 0);
}

static void search_registration(const char *state, const char *file_name, const cJSON *record,
                                runtime_changes_holders *holders) {
    size_t id_length = strlen(file_name) - REGISTRATION_SUFFIX_LENGTH;
    char *id = malloc(id_length + 1);
    if (id == NULL) {
        holders->failed = 1;
        return;
    }
    memcpy(id, file_name, id_length);
    id[id_length] = '\0';
    cJSON *registration = NULL;
    runtime_registry_answer loaded = runtime_registry_load(state, id, &registration);
    free(id);
    if (loaded == RUNTIME_REGISTRY_UNREADABLE) {
        holders->unreadable = 1;
    } else if (loaded == RUNTIME_REGISTRY_OUT_OF_MEMORY) {
        holders->failed = 1;
    } else if (loaded == RUNTIME_REGISTRY_OK && holder_in(registration, record) != NULL) {
        holders->holder = cJSON_Duplicate(holder_in(registration, record), 1);
        holders->failed = holders->holder == NULL;
    }
    cJSON_Delete(registration);
}

static void search_other_registrations(const char *state, const char *host, const cJSON *record,
                                       runtime_changes_holders *holders) {
    const char *segments[2] = {state, "hosts"};
    char *directory = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
    runtime_directory_listing listing;
    if (directory == NULL || !runtime_list_directory(directory, &listing)) {
        free(directory);
        holders->unreadable = 1;
        return;
    }
    free(directory);
    for (size_t index = 0; index < listing.count && holders->holder == NULL && !holders->failed; index++) {
        if (is_registration_of_another(&listing.entries[index], host)) {
            search_registration(state, listing.entries[index].name, record, holders);
        }
    }
    runtime_free_directory_listing(&listing);
}

runtime_changes_holders runtime_changes_find_holder(const char *state, const char *host, const cJSON *registration,
                                                    const cJSON *record) {
    runtime_changes_holders holders = {NULL, 0, 0};
    if (!runtime_change_is_shared(text_of(record, "kind"))) {
        return holders;
    }
    const cJSON *own = holder_in(registration, record);
    if (own != NULL) {
        holders.holder = cJSON_Duplicate(own, 1);
        holders.failed = holders.holder == NULL;
        return holders;
    }
    search_other_registrations(state, host, record, &holders);
    return holders;
}

static runtime_changes_outcome outcome_of(runtime_changes_answer answer, const char *reason) {
    runtime_changes_outcome outcome;
    outcome.answer = answer;
    outcome.sequence = 0;
    snprintf(outcome.reason, sizeof outcome.reason, "%s", reason);
    return outcome;
}

static void conclude(apply_session *session, runtime_changes_answer answer, const char *reason) {
    session->outcome = outcome_of(answer, reason);
}

static runtime_changes_outcome refusal_of_load(runtime_registry_answer loaded, const char *host) {
    char reason[1024];
    switch (loaded) {
    case RUNTIME_REGISTRY_ABSENT:
        snprintf(reason, sizeof reason, "the host %s is not registered, so its change could not be recorded", host);
        return outcome_of(RUNTIME_CHANGES_REFUSED, reason);
    case RUNTIME_REGISTRY_UNREADABLE:
        snprintf(reason, sizeof reason, "the registration of host %s is not readable; it was left unchanged", host);
        return outcome_of(RUNTIME_CHANGES_REFUSED, reason);
    case RUNTIME_REGISTRY_INVALID:
        return outcome_of(RUNTIME_CHANGES_REFUSED, "the host id is not a portable path segment");
    default:
        return outcome_of(RUNTIME_CHANGES_FAILED, "memory ran out");
    }
}

/**
 * @implNote The registration's nextSequence counter outlives the entries, so the number of a
 * withdrawn or dropped record is never handed out again; the highest stored sequence still counts
 * for a registration written before the counter existed.
 */
static long next_sequence(const cJSON *registration) {
    const cJSON *counter = cJSON_GetObjectItemCaseSensitive(registration, "nextSequence");
    long highest = cJSON_IsNumber(counter) ? (long)counter->valuedouble - 1 : 0;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, changes_of(registration)) {
        const cJSON *sequence = cJSON_GetObjectItemCaseSensitive(entry, "sequence");
        if (cJSON_IsNumber(sequence) && (long)sequence->valuedouble > highest) {
            highest = (long)sequence->valuedouble;
        }
    }
    return highest + 1;
}

static int move_members(cJSON *target, cJSON *source) {
    while (source->child != NULL) {
        cJSON *member = cJSON_DetachItemViaPointer(source, source->child);
        if (!cJSON_AddItemToObject(target, member->string, member)) {
            cJSON_Delete(member);
            return 0;
        }
    }
    return 1;
}

static cJSON *pending_entry(long sequence, const char *kind, cJSON *record) {
    cJSON *entry = cJSON_CreateObject();
    int complete = entry != NULL && cJSON_AddNumberToObject(entry, "sequence", (double)sequence) != NULL &&
                   cJSON_AddStringToObject(entry, "kind", kind) != NULL &&
                   cJSON_AddStringToObject(entry, "state", "pending") != NULL &&
                   cJSON_AddFalseToObject(entry, "reversed") != NULL && move_members(entry, record);
    cJSON_Delete(record);
    if (!complete) {
        cJSON_Delete(entry);
        return NULL;
    }
    return entry;
}

static int store(apply_session *session) {
    return runtime_registry_store(session->state, session->registration) == RUNTIME_REGISTRY_OK;
}

static void conclude_unwritable(apply_session *session) {
    char reason[1024];
    snprintf(reason, sizeof reason, "the registration of host %s could not be written under %s; nothing was changed",
             session->host, session->state);
    conclude(session, RUNTIME_CHANGES_FAILED, reason);
}

static void withdraw(apply_session *session, cJSON *entry, runtime_change_step step, const char *reason) {
    cJSON_Delete(cJSON_DetachItemViaPointer(changes_of(session->registration), entry));
    store(session);
    conclude(session, step == RUNTIME_CHANGE_REFUSED ? RUNTIME_CHANGES_REFUSED : RUNTIME_CHANGES_FAILED, reason);
}

/**
 * @implNote A record marked applied that could not be stored stays pending on the disk, and the next
 * apply or reverse completes it from the disk, so the change is answered RECORDED either way.
 */
static void perform_recorded(apply_session *session, cJSON *entry) {
    runtime_change_note note = {""};
    runtime_change_step step = runtime_change_perform(session->change, entry, &note);
    if (step != RUNTIME_CHANGE_DONE) {
        withdraw(session, entry, step, note.detail);
        return;
    }
    set_text(entry, "state", "applied");
    store(session);
    conclude(session, RUNTIME_CHANGES_RECORDED, "");
    session->outcome.sequence = (long)cJSON_GetObjectItemCaseSensitive(entry, "sequence")->valuedouble;
}

static int set_counter(cJSON *registration, long next) {
    cJSON *counter = cJSON_GetObjectItemCaseSensitive(registration, "nextSequence");
    if (counter != NULL) {
        cJSON_SetNumberValue(counter, (double)next);
        return 1;
    }
    return cJSON_AddNumberToObject(registration, "nextSequence", (double)next) != NULL;
}

static void record_and_perform(apply_session *session, cJSON *record) {
    long sequence = next_sequence(session->registration);
    cJSON *entry = pending_entry(sequence, text_of(session->change, "kind"), record);
    if (entry == NULL || !set_counter(session->registration, sequence + 1) ||
        !cJSON_AddItemToArray(changes_of(session->registration), entry)) {
        cJSON_Delete(entry);
        conclude(session, RUNTIME_CHANGES_FAILED, "memory ran out");
        return;
    }
    if (!store(session)) {
        conclude_unwritable(session);
        return;
    }
    perform_recorded(session, entry);
}

static int ensure_changes_list(cJSON *registration) {
    return cJSON_IsArray(changes_of(registration)) || cJSON_AddArrayToObject(registration, "changes") != NULL;
}

static void conclude_step(apply_session *session, runtime_change_step step, const char *reason) {
    conclude(session, step == RUNTIME_CHANGE_REFUSED ? RUNTIME_CHANGES_REFUSED : RUNTIME_CHANGES_FAILED, reason);
}

static long sequence_of(const cJSON *entry) {
    return (long)cJSON_GetObjectItemCaseSensitive(entry, "sequence")->valuedouble;
}

/**
 * This host's own unreversed record of what change asks for, or NULL.
 *
 * @implNote A host re-applies its changes on every start, so a repeat must answer the record it
 * already has; recording it again as held would grow the registration without bound.
 */
static const cJSON *own_record_of(const cJSON *registration, const cJSON *change) {
    const cJSON *entry;
    cJSON_ArrayForEach(entry, changes_of(registration)) {
        if (is_unreversed(entry) && runtime_change_repeats(entry, change)) {
            return entry;
        }
    }
    return NULL;
}

static void conclude_held_already(apply_session *session, const cJSON *own) {
    conclude(session, RUNTIME_CHANGES_UNNEEDED, "");
    session->outcome.sequence = sequence_of(own);
}

static void discard_prepared(prepared_children *prepared) {
    for (size_t index = 0; index < prepared->count; index++) {
        cJSON_Delete(prepared->items[index].record);
    }
    free(prepared->items);
}

static runtime_change_step prepare_child(apply_session *session, const cJSON *child, prepared_children *prepared,
                                         runtime_change_note *note) {
    runtime_changes_holders holders =
        runtime_changes_find_holder(session->state, session->host, session->registration, child);
    if (holders.failed) {
        snprintf(note->detail, sizeof note->detail, "memory ran out");
        return RUNTIME_CHANGE_FAILED;
    }
    cJSON *record = NULL;
    runtime_change_step step = runtime_change_prepare(child, holders.holder, &record, note);
    if (step == RUNTIME_CHANGE_DONE) {
        prepared_child *item = &prepared->items[prepared->count++];
        item->change = child;
        item->record = record;
        item->entry = NULL;
        item->held = holders.holder != NULL;
        item->undo_failed = 0;
    }
    cJSON_Delete(holders.holder);
    return step;
}

/**
 * Prepares every child this host does not already hold; answers 1 when none refused or failed, else
 * concludes the session and answers 0.
 */
static int prepare_children(apply_session *session, const cJSON *children, prepared_children *prepared) {
    prepared->count = 0;
    prepared->items = calloc((size_t)cJSON_GetArraySize(children) + 1, sizeof *prepared->items);
    if (prepared->items == NULL) {
        conclude(session, RUNTIME_CHANGES_FAILED, "memory ran out");
        return 0;
    }
    const cJSON *child;
    cJSON_ArrayForEach(child, children) {
        if (own_record_of(session->registration, child) != NULL) {
            continue;
        }
        runtime_change_note note = {""};
        runtime_change_step step = prepare_child(session, child, prepared, &note);
        if (step == RUNTIME_CHANGE_REFUSED || step == RUNTIME_CHANGE_FAILED) {
            conclude_step(session, step, note.detail);
            return 0;
        }
    }
    return 1;
}

static int add_child_entries(apply_session *session, long parent_sequence, long first_sequence,
                             prepared_children *prepared) {
    cJSON *changes = changes_of(session->registration);
    for (size_t index = 0; index < prepared->count; index++) {
        prepared_child *item = &prepared->items[index];
        item->entry = pending_entry(first_sequence + (long)index, text_of(item->change, "kind"), item->record);
        item->record = NULL;
        if (item->entry == NULL || cJSON_AddNumberToObject(item->entry, "parent", (double)parent_sequence) == NULL ||
            !cJSON_AddItemToArray(changes, item->entry)) {
            cJSON_Delete(item->entry);
            item->entry = NULL;
            return 0;
        }
    }
    return 1;
}

/** Withdraws the family, but keeps, as applied, every child whose undo failed and then its new parent. */
static void withdraw_family(apply_session *session, cJSON *new_parent, prepared_children *prepared) {
    cJSON *changes = changes_of(session->registration);
    int kept = 0;
    for (size_t index = 0; index < prepared->count; index++) {
        prepared_child *item = &prepared->items[index];
        if (item->entry != NULL && item->undo_failed) {
            set_text(item->entry, "state", "applied");
            kept = 1;
        } else if (item->entry != NULL) {
            cJSON_Delete(cJSON_DetachItemViaPointer(changes, item->entry));
        }
        item->entry = NULL;
    }
    if (new_parent != NULL && kept) {
        set_text(new_parent, "state", "applied");
    } else if (new_parent != NULL) {
        cJSON_Delete(cJSON_DetachItemViaPointer(changes, new_parent));
    }
    store(session);
}

/**
 * @implNote A child that cannot be performed undoes the children performed before it, under the same
 * lock and with the holders found when they were prepared, so a failed apply leaves neither a line
 * nor a record behind; a child whose undo fails is marked, so its record is kept with its line.
 */
static void undo_performed(prepared_children *prepared, size_t performed) {
    for (size_t index = performed; index > 0; index--) {
        prepared_child *item = &prepared->items[index - 1];
        runtime_change_note ignored = {""};
        item->undo_failed = runtime_change_reverse(item->entry, item->held, &ignored) != RUNTIME_CHANGE_REVERSED;
    }
}

static void mark_family_applied(cJSON *new_parent, prepared_children *prepared) {
    if (new_parent != NULL) {
        set_text(new_parent, "state", "applied");
    }
    for (size_t index = 0; index < prepared->count; index++) {
        set_text(prepared->items[index].entry, "state", "applied");
    }
}

static void perform_family(apply_session *session, cJSON *new_parent, long parent_sequence,
                           prepared_children *prepared) {
    runtime_change_note note = {""};
    runtime_change_step step = RUNTIME_CHANGE_DONE;
    size_t performed = 0;
    while (step == RUNTIME_CHANGE_DONE && performed < prepared->count) {
        step = runtime_change_perform(prepared->items[performed].change, prepared->items[performed].entry, &note);
        performed += step == RUNTIME_CHANGE_DONE;
    }
    if (step == RUNTIME_CHANGE_DONE && new_parent != NULL) {
        step = runtime_change_perform(session->change, new_parent, &note);
    }
    if (step != RUNTIME_CHANGE_DONE) {
        undo_performed(prepared, performed);
        withdraw_family(session, new_parent, prepared);
        conclude_step(session, step, note.detail);
        return;
    }
    mark_family_applied(new_parent, prepared);
    store(session);
    conclude(session, RUNTIME_CHANGES_RECORDED, "");
    session->outcome.sequence = parent_sequence;
}

/** Records the children, under a new parent made from parent_record, or under existing_parent when it is NULL. */
static void record_family(apply_session *session, cJSON *parent_record, long existing_parent,
                          prepared_children *prepared) {
    long next = next_sequence(session->registration);
    long parent_sequence = parent_record != NULL ? next : existing_parent;
    long first_child = parent_record != NULL ? next + 1 : next;
    cJSON *new_parent = NULL;
    if (parent_record != NULL) {
        new_parent = pending_entry(next, text_of(session->change, "kind"), parent_record);
        if (new_parent == NULL || !cJSON_AddItemToArray(changes_of(session->registration), new_parent)) {
            cJSON_Delete(new_parent);
            conclude(session, RUNTIME_CHANGES_FAILED, "memory ran out");
            return;
        }
    }
    if (!add_child_entries(session, parent_sequence, first_child, prepared) ||
        !set_counter(session->registration, first_child + (long)prepared->count)) {
        conclude(session, RUNTIME_CHANGES_FAILED, "memory ran out");
        return;
    }
    if (!store(session)) {
        conclude_unwritable(session);
        return;
    }
    perform_family(session, new_parent, parent_sequence, prepared);
}

static void record_new_family(apply_session *session, prepared_children *prepared) {
    cJSON *parent_record = NULL;
    runtime_change_note note = {""};
    runtime_change_step step = runtime_change_prepare(session->change, NULL, &parent_record, &note);
    if (step == RUNTIME_CHANGE_DONE) {
        record_family(session, parent_record, 0, prepared);
    } else if (step == RUNTIME_CHANGE_UNNEEDED) {
        conclude(session, RUNTIME_CHANGES_UNNEEDED, "");
    } else {
        conclude_step(session, step, note.detail);
    }
}

/**
 * @implNote The change and its children are stored pending together before any is performed, and
 * marked applied together after, so settling pending records treats each of them as it treats a
 * single change. Nothing is recorded when every child is already the user's or this host's; a child
 * new since this host's earlier apply joins that apply's parent.
 */
static void apply_with_children(apply_session *session, const cJSON *children) {
    const cJSON *own = own_record_of(session->registration, session->change);
    prepared_children prepared;
    if (!prepare_children(session, children, &prepared)) {
        discard_prepared(&prepared);
        return;
    }
    if (prepared.count == 0 && own != NULL) {
        conclude_held_already(session, own);
    } else if (prepared.count == 0) {
        conclude(session, RUNTIME_CHANGES_UNNEEDED, "");
    } else if (own != NULL) {
        record_family(session, NULL, sequence_of(own), &prepared);
    } else {
        record_new_family(session, &prepared);
    }
    discard_prepared(&prepared);
}

static void apply_single(apply_session *session) {
    const cJSON *own = own_record_of(session->registration, session->change);
    if (own != NULL) {
        conclude_held_already(session, own);
        return;
    }
    runtime_changes_holders holders =
        runtime_changes_find_holder(session->state, session->host, session->registration, session->change);
    if (holders.failed) {
        conclude(session, RUNTIME_CHANGES_FAILED, "memory ran out");
        return;
    }
    cJSON *record = NULL;
    runtime_change_note note = {""};
    runtime_change_step step = runtime_change_prepare(session->change, holders.holder, &record, &note);
    cJSON_Delete(holders.holder);
    if (step == RUNTIME_CHANGE_DONE) {
        record_and_perform(session, record);
    } else if (step == RUNTIME_CHANGE_UNNEEDED) {
        conclude(session, RUNTIME_CHANGES_UNNEEDED, "");
    } else {
        conclude_step(session, step, note.detail);
    }
}

static void prepare_and_apply(apply_session *session) {
    cJSON *children = NULL;
    runtime_change_note note = {""};
    runtime_change_step step = runtime_change_children(session->change, &children, &note);
    if (step != RUNTIME_CHANGE_DONE) {
        conclude_step(session, step, note.detail);
    } else if (children != NULL) {
        apply_with_children(session, children);
    } else {
        apply_single(session);
    }
    cJSON_Delete(children);
}

static runtime_changes_outcome apply_locked(const char *state, const char *host, const cJSON *change) {
    apply_session session = {state, host, change, NULL, {RUNTIME_CHANGES_FAILED, 0, ""}};
    runtime_registry_answer loaded = runtime_registry_load(state, host, &session.registration);
    if (loaded != RUNTIME_REGISTRY_OK) {
        return refusal_of_load(loaded, host);
    }
    if (!ensure_changes_list(session.registration)) {
        conclude(&session, RUNTIME_CHANGES_FAILED, "memory ran out");
    } else if (runtime_changes_settle_pending(session.registration) && !store(&session)) {
        conclude_unwritable(&session);
    } else {
        prepare_and_apply(&session);
    }
    cJSON_Delete(session.registration);
    return session.outcome;
}

runtime_changes_outcome runtime_changes_apply(const char *state, const char *host, const cJSON *change) {
    if (state == NULL || !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, state) || host == NULL) {
        return outcome_of(RUNTIME_CHANGES_REFUSED, "the state place must be an absolute path and the host named");
    }
    const char *defect = runtime_change_defect(change);
    if (defect != NULL) {
        return outcome_of(RUNTIME_CHANGES_REFUSED, defect);
    }
    cJSON *normal = runtime_change_normalized(change);
    if (normal == NULL) {
        return outcome_of(RUNTIME_CHANGES_FAILED, "memory ran out");
    }
    runtime_hosts_guard *guard = runtime_hosts_lock(state);
    runtime_changes_outcome outcome =
        guard == NULL ? outcome_of(RUNTIME_CHANGES_FAILED, "the registry lock under the state place could not be taken")
                      : apply_locked(state, host, normal);
    runtime_hosts_unlock(guard);
    cJSON_Delete(normal);
    return outcome;
}

static int32_t refuse_places(uint8_t **out, size_t *out_len) {
    return spisor_fail("the argument carries no usable places or host",
                       "send {\"places\": {\"state\": \"<absolute directory>\"}, \"host\": \"<host id>\"}", out,
                       out_len);
}

int32_t runtime_changes_serve(const uint8_t *in, size_t in_len, runtime_changes_handler handler, uint8_t **out,
                              size_t *out_len) {
    spisor_call call;
    int32_t status = spisor_parse_call(in, in_len, 0, &call, out, out_len);
    if (status != SPISOR_STATUS_SUCCESS) {
        return status;
    }
    const char *state = text_of(cJSON_GetObjectItemCaseSensitive(call.argument, "places"), "state");
    const char *host = text_of(call.argument, "host");
    if (state == NULL || !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, state) || host == NULL) {
        status = refuse_places(out, out_len);
    } else {
        status = handler(state, host, call.argument, out, out_len);
    }
    spisor_discard_call(&call);
    return status;
}

static int32_t answer_recorded(const runtime_changes_outcome *outcome, uint8_t **out, size_t *out_len) {
    cJSON *result = cJSON_CreateObject();
    int complete = result != NULL &&
                   cJSON_AddBoolToObject(result, "recorded", outcome->answer == RUNTIME_CHANGES_RECORDED) != NULL &&
                   (outcome->sequence <= 0 ||
                    cJSON_AddNumberToObject(result, "sequence", (double)outcome->sequence) != NULL);
    int32_t status = SPISOR_STATUS_OUT_OF_MEMORY;
    if (complete) {
        status = spisor_succeed(result, out, out_len);
    } else {
        *out = NULL;
        *out_len = 0;
    }
    cJSON_Delete(result);
    return status;
}

static int32_t apply_request(const char *state, const char *host, const cJSON *argument, uint8_t **out,
                             size_t *out_len) {
    runtime_changes_outcome outcome =
        runtime_changes_apply(state, host, cJSON_GetObjectItemCaseSensitive(argument, "change"));
    if (outcome.answer == RUNTIME_CHANGES_REFUSED) {
        return spisor_fail(outcome.reason, "nothing was changed or recorded", out, out_len);
    }
    if (outcome.answer == RUNTIME_CHANGES_FAILED) {
        return spisor_fail(outcome.reason, "nothing was recorded; fix the cause and apply the change again", out,
                           out_len);
    }
    return answer_recorded(&outcome, out, out_len);
}

RUNTIME_EXPORT int32_t runtime_changes_apply_json(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_changes_serve(in, in_len, apply_request, out, out_len);
}
