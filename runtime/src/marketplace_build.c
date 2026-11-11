#include "marketplace_build.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "identity.h"
#include "identity_resolve.h"
#include "import_name.h"
#include "json_service.h"
#include "marketplace_check.h"

#define MESSAGE_SIZE 1024
#define GITHUB_HOST "github.com"

typedef struct build_request {
    const cJSON *list;
    const char *built;
    const char *api_base;
    const char *token;
    const char *token_host;
    const cJSON *previous;
} build_request;

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

static int read_api_base(const cJSON *argument, const char **api_base, cJSON **refusal) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(argument, "apiBase");
    if (item != NULL && !cJSON_IsString(item)) {
        *refusal = runtime_refusal("apiBase is not text", "leave apiBase out: the host's own API address is used");
        return 0;
    }
    *api_base = item == NULL ? NULL : item->valuestring;
    return 1;
}

static int read_token(const cJSON *argument, const char **token, cJSON **refusal) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(argument, "token");
    if (item != NULL && !cJSON_IsString(item)) {
        *refusal = runtime_refusal("token is not text", "leave token out, or send the access token itself");
        return 0;
    }
    *token = item == NULL ? NULL : item->valuestring;
    return 1;
}

static int read_token_host(const cJSON *argument, const char **token_host, cJSON **refusal) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(argument, "tokenHost");
    if (item != NULL && (!cJSON_IsString(item) || item->valuestring[0] == '\0')) {
        *refusal = runtime_refusal("tokenHost is not a host name", "send the host the token belongs to, such as github.com");
        return 0;
    }
    *token_host = item == NULL ? NULL : item->valuestring;
    return 1;
}

static int check_token_is_usable(const build_request *request, cJSON **refusal) {
    if (request->token == NULL) {
        return 1;
    }
    if (request->token_host == NULL) {
        *refusal = runtime_refusal("a token was sent with no tokenHost",
                                   "send tokenHost: github.com for github ids, or the instance host for gitea@ and forgejo@ ids");
        return 0;
    }
    if (!runtime_identity_is_token_text(request->token)) {
        *refusal = runtime_refusal("the token is not usable", "send token as the access token itself, on one line with no spaces");
        return 0;
    }
    return 1;
}

static int read_previous(const cJSON *argument, const cJSON **previous, cJSON **refusal) {
    char problem[MESSAGE_SIZE / 2];
    *previous = cJSON_GetObjectItemCaseSensitive(argument, "previous");
    if (*previous != NULL && !runtime_marketplace_check_copy(*previous, problem, sizeof problem)) {
        refuse_with(refusal, "send the copy this build replaces as marketplace-copy.schema.json describes it, or leave previous out",
                    "previous is not a marketplace copy: %s", problem, "", "");
        return 0;
    }
    return 1;
}

static int read_request(const cJSON *argument, build_request *request, cJSON **refusal) {
    char problem[MESSAGE_SIZE / 2];
    request->list = cJSON_GetObjectItemCaseSensitive(argument, "list");
    request->built = text_of(argument, "built");
    if (!read_api_base(argument, &request->api_base, refusal) || !read_token(argument, &request->token, refusal) ||
        !read_token_host(argument, &request->token_host, refusal) || !check_token_is_usable(request, refusal) ||
        !read_previous(argument, &request->previous, refusal)) {
        return 0;
    }
    if (request->built == NULL || !runtime_marketplace_is_built_time(request->built)) {
        *refusal = runtime_refusal("built is not an RFC 3339 timestamp",
                                   "send the time of the build as RFC 3339, for example \"2026-11-10T12:00:00Z\"");
        return 0;
    }
    if (!runtime_marketplace_check_list(request->list, problem, sizeof problem)) {
        refuse_with(refusal, "write the list as marketplace.schema.json describes", "the list is not a marketplace list: %s",
                    problem, "", "");
        return 0;
    }
    return 1;
}

static int is_local_id(const runtime_identity *parsed) {
    return parsed->resolver.length == 5 && strncmp(parsed->resolver.start, "local", 5) == 0;
}

static int is_listed_before(const cJSON *entries, const cJSON *entry, const char *id) {
    for (const cJSON *earlier = entries->child; earlier != NULL && earlier != entry; earlier = earlier->next) {
        if (strcmp(text_of(earlier, "id"), id) == 0) {
            return 1;
        }
    }
    return 0;
}

static int check_listed_id(const cJSON *entries, const cJSON *entry, cJSON **refusal) {
    const char *id = text_of(entry, "id");
    runtime_identity parsed;
    if (!runtime_identity_parse_id(id, &parsed)) {
        refuse_with(refusal, "list each repository by its host id, such as github:123", "%s is not a repository id", id, "", "");
    } else if (is_local_id(&parsed)) {
        refuse_with(refusal, "list a repository by its host id, such as github:123; a local id has no host to look up",
                    "%s is a local id", id, "", "");
    } else if (is_listed_before(entries, entry, id)) {
        refuse_with(refusal, "list each repository once", "%s is listed twice", id, "", "");
    } else {
        return 1;
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

static int equals_ignoring_case(const char *text, const char *start, size_t length) {
    if (strlen(text) != length) {
        return 0;
    }
    for (size_t index = 0; index < length; index++) {
        if (tolower((unsigned char)text[index]) != tolower((unsigned char)start[index])) {
            return 0;
        }
    }
    return 1;
}

static int is_github_id(const runtime_identity *parsed) {
    return parsed->resolver.length == 6 && strncmp(parsed->resolver.start, "github", 6) == 0;
}

/** The host an id carries, or github.com for a github id that carries none; an id with neither is on no host. */
static int is_on_host(const runtime_identity *parsed, const char *host) {
    if (parsed->host.start != NULL) {
        return equals_ignoring_case(host, parsed->host.start, parsed->host.length);
    }
    return is_github_id(parsed) && equals_ignoring_case(host, GITHUB_HOST, strlen(GITHUB_HOST));
}

/** One token belongs to one host, so a list that sends it must name no other host for it to reach. */
static int check_ids_are_on_token_host(const cJSON *entries, const char *token_host, cJSON **refusal) {
    for (const cJSON *entry = entries->child; entry != NULL; entry = entry->next) {
        runtime_identity parsed;
        if (runtime_identity_parse_id(text_of(entry, "id"), &parsed) && !is_on_host(&parsed, token_host)) {
            refuse_with(refusal, "list only ids on the token's host, or build this list without a token",
                        "%s is not on %s, the one host the token is sent to", text_of(entry, "id"), token_host, "");
            return 0;
        }
    }
    return 1;
}

static cJSON *resolve_argument(const char *id, const build_request *request) {
    cJSON *argument = cJSON_CreateObject();
    if (argument == NULL || cJSON_AddStringToObject(argument, "id", id) == NULL ||
        (request->api_base != NULL && cJSON_AddStringToObject(argument, "apiBase", request->api_base) == NULL) ||
        (request->token != NULL && cJSON_AddStringToObject(argument, "token", request->token) == NULL)) {
        cJSON_Delete(argument);
        return NULL;
    }
    return argument;
}

/**
 * What identity.resolve answered for one repository: answer, or NULL with reason saying why the host could not be
 * asked. reason stays empty when the failure was memory running out, which is not the host's to answer for.
 */
typedef struct lookup_result {
    cJSON *answer;
    char reason[MESSAGE_SIZE];
    long status;
} lookup_result;

static lookup_result resolved(const char *repository_id, const build_request *request) {
    lookup_result result = {NULL, "", 0};
    cJSON *lookup_refusal = NULL;
    result.answer = runtime_call_or_refuse(runtime_identity_resolve, resolve_argument(repository_id, request), &lookup_refusal);
    if (result.answer == NULL && runtime_is_refusal(lookup_refusal)) {
        snprintf(result.reason, sizeof result.reason, "it cannot be looked up: %s", text_of(lookup_refusal, "message"));
        result.status = runtime_identity_refusal_status(lookup_refusal);
    }
    cJSON_Delete(lookup_refusal);
    return result;
}

/** The id of the repository itself: id up to the folder below it, freed with free; NULL when memory runs out. */
static char *repository_id_of(const char *id) {
    size_t length = strcspn(id, "/");
    char *repository = malloc(length + 1);
    if (repository != NULL) {
        memcpy(repository, id, length);
        repository[length] = '\0';
    }
    return repository;
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
 * separators reduced to its first, and none left at either end. Freed with free; NULL when memory runs out. The
 * result can still be no import name (empty, or reserved), which the caller checks.
 */
static char *derived_name(const char *text) {
    char *name = malloc(strlen(text) + 1);
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
        char *copy = malloc(strlen(alias) + 1);
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
        cJSON_AddStringToObject(copy, "name", name) == NULL || cJSON_AddStringToObject(copy, "fullName", full_name) == NULL ||
        cJSON_AddStringToObject(copy, "lastKnown", full_name) == NULL || !copies_member(copy, entry, "description") ||
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
static entry_result left_unlooked(const build_request *request, const cJSON *entry, const char *reason, cJSON *skipped) {
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

/** The copy of one entry from what its repository answered; a name that is no import name skips it. */
static entry_result copied_from(const cJSON *entry, const cJSON *answer, cJSON *skipped) {
    entry_result result = {ENTRY_FAILED, NULL, 0};
    const char *id = text_of(entry, "id");
    const char *folder = folder_name_of(id);
    const char *source_name = folder != NULL ? folder : text_of(answer, "name");
    const char *full_name = text_of(answer, "fullName");
    char *name = source_name == NULL ? NULL : shown_name(entry, source_name);
    if (name != NULL && runtime_is_import_name(name)) {
        result.copy = copy_entry(entry, name, full_name);
        result.outcome = result.copy == NULL ? ENTRY_FAILED : ENTRY_COPIED;
    } else if (name != NULL && adds_skipped_for_name(skipped, id, source_name)) {
        result.outcome = ENTRY_SKIPPED;
    }
    free(name);
    return result;
}

/** The copy of one list entry as its repository is now; one that cannot be looked up is carried over or skipped. */
static entry_result copied_entry(const build_request *request, const cJSON *entry, cJSON *skipped) {
    entry_result result = {ENTRY_FAILED, NULL, 0};
    char *repository_id = repository_id_of(text_of(entry, "id"));
    lookup_result found = {NULL, "", 0};
    if (repository_id != NULL) {
        found = resolved(repository_id, request);
    }
    const char *full_name = found.answer == NULL ? NULL : text_of(found.answer, "fullName");
    if (found.answer != NULL && (full_name == NULL || !runtime_marketplace_is_full_name(full_name))) {
        snprintf(found.reason, sizeof found.reason, "it cannot be looked up: the host answered a name that is not owner/name");
        cJSON_Delete(found.answer);
        found.answer = NULL;
    }
    if (found.answer != NULL) {
        result = copied_from(entry, found.answer, skipped);
    } else if (found.reason[0] != '\0') {
        result = left_unlooked(request, entry, found.reason, skipped);
    }
    result.was_rate_limited = found.status == 429 || found.status == 403;
    cJSON_Delete(found.answer);
    free(repository_id);
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

static cJSON *copied_entries(const build_request *request, const cJSON *entries, cJSON *skipped, cJSON **refusal) {
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

static cJSON *copy_of(const build_request *request, cJSON *entries) {
    cJSON *copy = cJSON_CreateObject();
    const cJSON *title = cJSON_GetObjectItemCaseSensitive(request->list, "title");
    if (copy == NULL || cJSON_AddNumberToObject(copy, "format", 1) == NULL ||
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

static cJSON *build(const cJSON *argument, cJSON **refusal) {
    build_request request;
    if (!read_request(argument, &request, refusal)) {
        return NULL;
    }
    const cJSON *entries = cJSON_GetObjectItemCaseSensitive(request.list, "entries");
    if (!check_listed_ids(entries, refusal) || (request.token != NULL && !check_ids_are_on_token_host(entries, request.token_host, refusal))) {
        return NULL;
    }
    cJSON *skipped = cJSON_CreateArray();
    cJSON *copied = skipped == NULL ? NULL : copied_entries(&request, entries, skipped, refusal);
    if (copied == NULL) {
        cJSON_Delete(skipped);
        return NULL;
    }
    return answer_of(copy_of(&request, copied), skipped);
}

RUNTIME_EXPORT int32_t runtime_marketplace_build(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, build);
}
