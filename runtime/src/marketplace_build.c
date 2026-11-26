#include "marketplace_build.h"
#include "allocation.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "definition_support.h"
#include "identity.h"
#include "identity_naula.h"
#include "import_name.h"
#include "json_service.h"
#include "marketplace_check.h"

#define MESSAGE_SIZE 1024
#define GITHUB_HOST "github.com"
#define GITHUB_WORD "github"

static void refuse_with(cJSON **refusal, const char *fix, const char *format, const char *first, const char *second,
                        const char *third) {
    char message[MESSAGE_SIZE];
    snprintf(message, sizeof message, format, first, second, third);
    *refusal = runtime_refusal(message, fix);
}

static const char *text_of(const cJSON *object, const char *member) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, member);
    return cJSON_IsString(value) ? value->valuestring : NULL;
}

static int check_token_host(const char *token_host, cJSON **refusal) {
    if (token_host != NULL && token_host[0] == '\0') {
        *refusal = runtime_refusal("token_host is not a host name",
                                   "set token_host to the host the token belongs to, such as github.com");
        return 0;
    }
    return 1;
}

static int check_token_is_usable(const runtime_marketplace_build_request *request, cJSON **refusal) {
    if (request->token == NULL) {
        return 1;
    }
    if (request->token_host == NULL) {
        *refusal = runtime_refusal("a token was sent with no token_host",
                                   "set token_host to github.com for places on GitHub, or to the instance host of "
                                   "places on another host");
        return 0;
    }
    if (!runtime_identity_is_token_text(request->token)) {
        *refusal = runtime_refusal("the token is not usable", "send token as the access token itself, on one line with no spaces");
        return 0;
    }
    return 1;
}

/** 1 for no previous copy or a usable one; 0 with a refusal, or without one when memory runs out. */
static int check_previous(const cJSON *previous, cJSON **refusal) {
    char problem[MESSAGE_SIZE / 2];
    int verdict = previous == NULL ? 1 : runtime_marketplace_check_copy(previous, problem, sizeof problem);
    if (verdict == 0) {
        refuse_with(refusal, "send the copy this build replaces as marketplace-copy.schema.json describes it, or leave previous out",
                    "previous is not a marketplace copy: %s", problem, "", "");
    }
    return verdict == 1;
}

static int is_usable_request(const runtime_marketplace_build_request *request, cJSON **refusal) {
    char problem[MESSAGE_SIZE / 2];
    if (!check_token_host(request->token_host, refusal) || !check_token_is_usable(request, refusal) ||
        !check_previous(request->previous, refusal)) {
        return 0;
    }
    if (request->built == NULL || !runtime_marketplace_is_built_time(request->built)) {
        *refusal = runtime_refusal("built is not an RFC 3339 timestamp",
                                   "send the time of the build as RFC 3339, for example \"2026-11-10T12:00:00Z\"");
        return 0;
    }
    int verdict = runtime_marketplace_check_list(request->list, problem, sizeof problem);
    if (verdict == 0) {
        refuse_with(refusal, "write the list as marketplace.schema.json describes", "the list is not a marketplace list: %s",
                    problem, "", "");
    }
    return verdict == 1;
}

static int is_listed_before(const cJSON *entries, const cJSON *entry, const char *id) {
    for (const cJSON *earlier = entries->child; earlier != NULL && earlier != entry; earlier = earlier->next) {
        if (strcmp(text_of(earlier, "id"), id) == 0) {
            return 1;
        }
    }
    return 0;
}

/** 1 for an entry whose id may be listed; 0 with a refusal, or without one when memory runs out. */
static int check_listed_id(const cJSON *entries, const cJSON *entry, cJSON **refusal) {
    const char *id = text_of(entry, "id");
    runtime_identity_kind kind = runtime_identity_kind_of(id);
    if (kind == RUNTIME_IDENTITY_NOT_AN_ID) {
        refuse_with(refusal, "list each repository by its id, such as git:<root commit>", "%s is not a repository id", id,
                    "", "");
    } else if (kind == RUNTIME_IDENTITY_LOCAL) {
        refuse_with(refusal, "list a repository by its git: id; a local id has no home to resolve", "%s is a local id",
                    id, "", "");
    } else if (kind == RUNTIME_IDENTITY_GIT && is_listed_before(entries, entry, id)) {
        refuse_with(refusal, "list each repository once", "%s is listed twice", id, "", "");
    } else {
        return kind == RUNTIME_IDENTITY_GIT;
    }
    return 0;
}

static int check_listed_ids(const cJSON *entries, cJSON **refusal) {
    for (const cJSON *entry = entries->child; entry != NULL; entry = entry->next) {
        if (!check_listed_id(entries, entry, refusal)) {
            return 0;
        }
    }
    return 1;
}

static int equals_ignoring_case(const char *left, const char *right) {
    if (strlen(left) != strlen(right)) {
        return 0;
    }
    for (size_t index = 0; left[index] != '\0'; index++) {
        if (tolower((unsigned char)left[index]) != tolower((unsigned char)right[index])) {
            return 0;
        }
    }
    return 1;
}

/** The host a place is asked at: its own, github.com for a github place that names none, a web place's domain. */
static const char *host_of(const naula_place *place) {
    if (place->kind == NAULA_HOME_PROVIDER) {
        return place->host != NULL ? place->host : strcmp(place->provider, GITHUB_WORD) == 0 ? GITHUB_HOST : NULL;
    }
    return place->kind == NAULA_HOME_WEB ? place->domain : NULL;
}

/** 1 when the place is on host, 0 when it is not or names none, -1 when memory runs out. */
static int is_on_host(const char *text, const char *host) {
    naula_place place;
    memset(&place, 0, sizeof place);
    place.size = sizeof place;
    naula_status status = naula_place_parse(text, &place, NULL);
    const char *named = status == NAULA_OK ? host_of(&place) : NULL;
    int on = named != NULL && equals_ignoring_case(named, host);
    naula_place_release(&place);
    return status == NAULA_OUT_OF_MEMORY ? -1 : on;
}

/** One token belongs to one host, so a list that sends it must name no other host for it to reach. */
static int check_places_are_on_token_host(const cJSON *entries, const char *token_host, cJSON **refusal) {
    for (const cJSON *entry = entries->child; entry != NULL; entry = entry->next) {
        const cJSON *place;
        cJSON_ArrayForEach(place, cJSON_GetObjectItemCaseSensitive(entry, "at")) {
            int on = is_on_host(place->valuestring, token_host);
            if (on == 0) {
                refuse_with(refusal, "list only places on the token's host, or build this list without a token",
                            "%s of %s is not on %s, the one host the token is sent to", place->valuestring,
                            text_of(entry, "id"), token_host);
            }
            if (on != 1) {
                return 0;
            }
        }
    }
    return 1;
}

/**
 * What naula answered for one repository: its primary's `name` and `full_name`, or `found` 0 with reason saying why it
 * could not be confirmed. `out_of_memory` is set when memory ran out, which is not the host's to answer for.
 */
typedef struct lookup_result {
    int found;
    int out_of_memory;
    char name[MESSAGE_SIZE / 4];
    char full_name[MESSAGE_SIZE / 2];
    char reason[MESSAGE_SIZE];
    int was_rate_limited;
} lookup_result;

static int is_rate_limited(const naula_refusal *refused) {
    return strstr(refused->code, "rate_limited") != NULL || strstr(refused->detail, "rate_limited") != NULL;
}

static void take_primary(const naula_resolution *resolution, lookup_result *result) {
    const naula_repository *primary = &resolution->primary_repository;
    if (resolution->state != NAULA_CONFIRMED || primary->account == NULL || primary->name == NULL) {
        snprintf(result->reason, sizeof result->reason,
                 "it has no owner/name to show: its primary is not a repository on a provider");
        return;
    }
    snprintf(result->name, sizeof result->name, "%s", primary->name);
    snprintf(result->full_name, sizeof result->full_name, "%s/%s", primary->account, primary->name);
    result->found = 1;
}

/** The texts of the entry's places, freed; NULL when memory runs out. */
static const char **places_of(const cJSON *entry, size_t *count) {
    const cJSON *at = cJSON_GetObjectItemCaseSensitive(entry, "at");
    *count = (size_t)cJSON_GetArraySize(at);
    const char **places = spisor_memory_allocate((*count == 0 ? 1 : *count) * sizeof *places);
    size_t index = 0;
    const cJSON *place;
    cJSON_ArrayForEach(place, at) {
        if (places != NULL) {
            places[index++] = place->valuestring;
        }
    }
    return places;
}

static lookup_result resolved(const char *repository_id, const cJSON *entry,
                              const runtime_marketplace_build_request *request) {
    lookup_result result;
    memset(&result, 0, sizeof result);
    size_t count = 0;
    const char **places = places_of(entry, &count);
    if (places == NULL) {
        result.out_of_memory = 1;
        return result;
    }
    runtime_identity_request resolving = {repository_id, places, count, NULL, request->token, request->token_host,
                                          NULL, NULL};
    naula_resolution resolution;
    memset(&resolution, 0, sizeof resolution);
    resolution.size = sizeof resolution;
    runtime_identity_acceptance acceptance;
    naula_refusal refused;
    memset(&refused, 0, sizeof refused);
    refused.size = sizeof refused;
    naula_status status = runtime_identity_resolve(&resolving, &resolution, &acceptance, &refused);
    if (status == NAULA_OK) {
        take_primary(&resolution, &result);
    } else if (status == NAULA_OUT_OF_MEMORY) {
        result.out_of_memory = 1;
    } else {
        char text[MESSAGE_SIZE / 2];
        runtime_identity_refusal_text(&refused, text, sizeof text);
        snprintf(result.reason, sizeof result.reason, "it cannot be resolved: %s", text);
        result.was_rate_limited = is_rate_limited(&refused);
    }
    naula_resolution_release(&resolution);
    spisor_memory_release(places);
    return result;
}

/** The last segment of the folder id names below its repository, or NULL when id names the repository itself. */
static const char *folder_name_of(const char *id) {
    const char *last_slash = strrchr(id, '/');
    return last_slash == NULL ? NULL : last_slash + 1;
}

static char lower_of(char character) {
    return (char)tolower((unsigned char)character);
}

static int is_word_character(char character) {
    return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
}

/**
 * text as an import name: lowercased, each run of characters outside [a-z0-9.-] turned into one "-", every run of
 * separators reduced to its first, and none left at either end. Freed; NULL when memory runs out. The
 * result can still be no import name (empty, or reserved), which the caller checks.
 */
static char *derived_name(const char *text) {
    char *name = spisor_memory_allocate(strlen(text) + 1);
    size_t used = 0;
    char pending = '\0';
    for (size_t index = 0; name != NULL && text[index] != '\0'; index++) {
        char character = lower_of(text[index]);
        if (is_word_character(character)) {
            if (pending != '\0' && used > 0) {
                name[used++] = pending;
            }
            pending = '\0';
            name[used++] = character;
        } else if (pending == '\0') {
            pending = character == '.' ? '.' : '-';
        }
    }
    if (name != NULL) {
        name[used] = '\0';
    }
    return name;
}

/** The name entry is shown under before it is checked: its alias when it has one, otherwise one derived from the source. */
static char *shown_name(const cJSON *entry, const char *source_name) {
    const char *alias = text_of(entry, "alias");
    if (alias != NULL) {
        char *copy = spisor_memory_allocate(strlen(alias) + 1);
        return copy == NULL ? NULL : strcpy(copy, alias);
    }
    return derived_name(source_name);
}

static int copies_member(cJSON *to, const cJSON *from, const char *member) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(from, member);
    return value == NULL || cJSON_AddItemToObject(to, member, cJSON_Duplicate(value, 1)) != 0;
}

static cJSON *copy_entry(const cJSON *entry, const char *name, const char *full_name) {
    cJSON *copy = cJSON_CreateObject();
    if (copy == NULL || cJSON_AddStringToObject(copy, "id", text_of(entry, "id")) == NULL ||
        !copies_member(copy, entry, "at") || cJSON_AddStringToObject(copy, "name", name) == NULL ||
        cJSON_AddStringToObject(copy, "fullName", full_name) == NULL || !copies_member(copy, entry, "description") ||
        !copies_member(copy, entry, "categories")) {
        cJSON_Delete(copy);
        return NULL;
    }
    return copy;
}

static int check_name_is_free(const cJSON *copied, const cJSON *entry, cJSON **refusal) {
    const char *name = text_of(entry, "name");
    for (const cJSON *earlier = copied->child; earlier != NULL; earlier = earlier->next) {
        if (strcmp(text_of(earlier, "name"), name) == 0) {
            refuse_with(refusal, "set an alias on one of them in the list so each entry has its own name",
                        "%s and %s are both named %s", text_of(earlier, "id"), text_of(entry, "id"), name);
            return 0;
        }
    }
    return 1;
}

static int adds_skipped(cJSON *skipped, const char *id, const char *reason) {
    cJSON *row = cJSON_CreateObject();
    if (row == NULL || cJSON_AddStringToObject(row, "id", id) == NULL || cJSON_AddStringToObject(row, "reason", reason) == NULL ||
        !cJSON_AddItemToArray(skipped, row)) {
        cJSON_Delete(row);
        return 0;
    }
    return 1;
}

typedef enum entry_outcome { ENTRY_COPIED, ENTRY_SKIPPED, ENTRY_FAILED } entry_outcome;

typedef struct entry_result {
    entry_outcome outcome;
    cJSON *copy;
    int was_rate_limited;
} entry_result;

static const cJSON *previous_entry_of(const cJSON *previous, const char *id) {
    const cJSON *entries = previous == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(previous, "entries");
    const cJSON *entry;
    cJSON_ArrayForEach(entry, entries) {
        if (strcmp(text_of(entry, "id"), id) == 0) {
            return entry;
        }
    }
    return NULL;
}

/** An entry whose repository could not be looked up: the previous copy's entry unchanged when it has one, otherwise skipped. */
static entry_result left_unlooked(const runtime_marketplace_build_request *request, const cJSON *entry,
                                  const char *reason, cJSON *skipped) {
    entry_result result = {ENTRY_FAILED, NULL, 0};
    const char *id = text_of(entry, "id");
    const cJSON *kept = previous_entry_of(request->previous, id);
    if (kept != NULL) {
        result.copy = cJSON_Duplicate(kept, 1);
        result.outcome = result.copy == NULL ? ENTRY_FAILED : ENTRY_COPIED;
    } else if (adds_skipped(skipped, id, reason)) {
        result.outcome = ENTRY_SKIPPED;
    }
    return result;
}

static int adds_skipped_for_name(cJSON *skipped, const char *id, const char *source_name) {
    char reason[MESSAGE_SIZE];
    snprintf(reason, sizeof reason, "its name \"%s\" cannot be made an import name; set an alias in the list to list it",
             source_name);
    return adds_skipped(skipped, id, reason);
}

/** The copy of one entry from what its repository's primary answered; a name that is no import name skips it. */
static entry_result copied_from(const cJSON *entry, const lookup_result *found, cJSON *skipped) {
    entry_result result = {ENTRY_FAILED, NULL, 0};
    const char *id = text_of(entry, "id");
    const char *folder = folder_name_of(id);
    const char *source_name = folder != NULL ? folder : found->name;
    const char *full_name = found->full_name;
    char *name = shown_name(entry, source_name);
    if (name != NULL && runtime_is_import_name(name)) {
        result.copy = copy_entry(entry, name, full_name);
        result.outcome = result.copy == NULL ? ENTRY_FAILED : ENTRY_COPIED;
    } else if (name != NULL && adds_skipped_for_name(skipped, id, source_name)) {
        result.outcome = ENTRY_SKIPPED;
    }
    spisor_memory_release(name);
    return result;
}

/** The copy of one list entry as its repository is now; one naula does not confirm is carried over or skipped. */
static entry_result copied_entry(const runtime_marketplace_build_request *request, const cJSON *entry, cJSON *skipped) {
    entry_result result = {ENTRY_FAILED, NULL, 0};
    char *repository_id = runtime_identity_repo_of(text_of(entry, "id"));
    lookup_result found;
    memset(&found, 0, sizeof found);
    found.out_of_memory = repository_id == NULL;
    if (repository_id != NULL) {
        found = resolved(repository_id, entry, request);
    }
    if (found.found && !runtime_marketplace_is_full_name(found.full_name)) {
        snprintf(found.reason, sizeof found.reason, "it cannot be resolved: the host answered a name that is not owner/name");
        found.found = 0;
    }
    if (found.found) {
        result = copied_from(entry, &found, skipped);
    } else if (!found.out_of_memory) {
        result = left_unlooked(request, entry, found.reason, skipped);
    }
    result.was_rate_limited = found.was_rate_limited;
    spisor_memory_release(repository_id);
    return result;
}

static int adds_copied_entry(cJSON *copied, cJSON *copy, cJSON **refusal) {
    return check_name_is_free(copied, copy, refusal) && cJSON_AddItemToArray(copied, copy);
}

#define REASONS_NAMED 3

static void append_reason(char *message, size_t size, const cJSON *row, int position) {
    size_t used = strlen(message);
    snprintf(message + used, size - used, "%s%s: %s", position == 0 ? "" : "; ", text_of(row, "id"), text_of(row, "reason"));
}

/** A list with entries whose copy holds none is refused with the reasons seen, since an empty copy would be published as true. */
static void refuse_empty_copy(const cJSON *skipped, int is_every_entry_rate_limited, cJSON **refusal) {
    char message[MESSAGE_SIZE] = "no listed repository could be built: ";
    int position = 0;
    for (const cJSON *row = skipped->child; row != NULL && position < REASONS_NAMED; row = row->next) {
        append_reason(message, sizeof message, row, position++);
    }
    if (cJSON_GetArraySize(skipped) > REASONS_NAMED) {
        size_t used = strlen(message);
        snprintf(message + used, sizeof message - used, "; and %d more", cJSON_GetArraySize(skipped) - REASONS_NAMED);
    }
    *refusal = runtime_refusal(message, is_every_entry_rate_limited
                                            ? "the host is rate limiting this caller: wait for its limit to reset, or send an access token; nothing was written"
                                            : "fix the reasons named, or remove the ids that no longer resolve from the list; nothing was written");
}

static cJSON *copied_entries(const runtime_marketplace_build_request *request, const cJSON *entries, cJSON *skipped,
                             cJSON **refusal) {
    cJSON *copied = cJSON_CreateArray();
    int rate_limited_count = 0;
    for (const cJSON *entry = entries->child; copied != NULL && entry != NULL; entry = entry->next) {
        entry_result result = copied_entry(request, entry, skipped);
        rate_limited_count += result.was_rate_limited;
        if (result.outcome == ENTRY_SKIPPED) {
            continue;
        }
        if (result.outcome == ENTRY_FAILED || !adds_copied_entry(copied, result.copy, refusal)) {
            cJSON_Delete(result.copy);
            cJSON_Delete(copied);
            return NULL;
        }
    }
    if (copied != NULL && entries->child != NULL && cJSON_GetArraySize(copied) == 0) {
        refuse_empty_copy(skipped, rate_limited_count == cJSON_GetArraySize(entries), refusal);
        cJSON_Delete(copied);
        return NULL;
    }
    return copied;
}

static cJSON *copy_of(const runtime_marketplace_build_request *request, cJSON *entries) {
    cJSON *copy = cJSON_CreateObject();
    const cJSON *title = cJSON_GetObjectItemCaseSensitive(request->list, "title");
    if (copy == NULL || cJSON_AddNumberToObject(copy, "format", RUNTIME_MARKETPLACE_FORMAT) == NULL ||
        (title != NULL && cJSON_AddStringToObject(copy, "title", title->valuestring) == NULL) ||
        cJSON_AddStringToObject(copy, "built", request->built) == NULL || !cJSON_AddItemToObject(copy, "entries", entries)) {
        cJSON_Delete(copy);
        cJSON_Delete(entries);
        return NULL;
    }
    return copy;
}

static cJSON *answer_of(cJSON *copy, cJSON *skipped) {
    cJSON *answer = cJSON_CreateObject();
    if (copy == NULL || answer == NULL || !cJSON_AddItemToObject(answer, "copy", copy) ||
        !cJSON_AddItemToObject(answer, "skipped", skipped)) {
        cJSON_Delete(copy);
        cJSON_Delete(skipped);
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

cJSON *runtime_marketplace_build(const runtime_marketplace_build_request *request, cJSON **refusal) {
    *refusal = NULL;
    if (!is_usable_request(request, refusal)) {
        return NULL;
    }
    const cJSON *entries = cJSON_GetObjectItemCaseSensitive(request->list, "entries");
    if (!check_listed_ids(entries, refusal) ||
        (request->token != NULL && !check_places_are_on_token_host(entries, request->token_host, refusal))) {
        return NULL;
    }
    cJSON *skipped = cJSON_CreateArray();
    cJSON *copied = skipped == NULL ? NULL : copied_entries(request, entries, skipped, refusal);
    if (copied == NULL) {
        cJSON_Delete(skipped);
        return NULL;
    }
    return answer_of(copy_of(request, copied), skipped);
}
