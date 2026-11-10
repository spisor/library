#include "marketplace_build.h"

#include <stdio.h>
#include <string.h>

#include "identity.h"
#include "identity_resolve.h"
#include "import_name.h"
#include "json_service.h"
#include "marketplace_check.h"

#define MESSAGE_SIZE 1024

typedef struct build_request {
    const cJSON *list;
    const char *built;
    const char *api_base;
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

static int read_request(const cJSON *argument, build_request *request, cJSON **refusal) {
    char problem[MESSAGE_SIZE / 2];
    request->list = cJSON_GetObjectItemCaseSensitive(argument, "list");
    request->built = text_of(argument, "built");
    if (!read_api_base(argument, &request->api_base, refusal)) {
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
    } else if (parsed.path.length > 0) {
        refuse_with(refusal, "list the repository itself, such as github:123, without a folder below it",
                    "%s names a folder below a repository", id, "", "");
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

static cJSON *resolve_argument(const char *id, const char *api_base) {
    cJSON *argument = cJSON_CreateObject();
    if (argument == NULL || cJSON_AddStringToObject(argument, "id", id) == NULL ||
        (api_base != NULL && cJSON_AddStringToObject(argument, "apiBase", api_base) == NULL)) {
        cJSON_Delete(argument);
        return NULL;
    }
    return argument;
}

/** What identity.resolve answers for id, or NULL with *refusal naming id and the resolve refusal's fix. */
static cJSON *resolved(const char *id, const char *api_base, cJSON **refusal) {
    cJSON *lookup_refusal = NULL;
    cJSON *answer = runtime_call_or_refuse(runtime_identity_resolve, resolve_argument(id, api_base), &lookup_refusal);
    if (answer == NULL && runtime_is_refusal(lookup_refusal)) {
        refuse_with(refusal, text_of(lookup_refusal, "fix"), "%s cannot be looked up: %s", id, text_of(lookup_refusal, "message"), "");
    }
    cJSON_Delete(lookup_refusal);
    return answer;
}

/** The name entry is shown under: its alias when it has one, otherwise the repository's own. */
static const char *shown_name(const cJSON *entry, const cJSON *answer) {
    const char *alias = text_of(entry, "alias");
    return alias != NULL ? alias : text_of(answer, "name");
}

static int check_answer(const char *id, const char *name, const char *full_name, cJSON **refusal) {
    if (full_name == NULL || !runtime_marketplace_is_full_name(full_name)) {
        refuse_with(refusal, "check that the host answers an owner/name", "%s was answered a name that is not owner/name", id, "", "");
    } else if (name == NULL || !runtime_is_import_name(name)) {
        refuse_with(refusal, "set an alias for this entry in the list, written as an import name",
                    "%s is named \"%s\", which is not an import name", id, name == NULL ? "" : name, "");
    } else {
        return 1;
    }
    return 0;
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

/** The copy of one list entry as the repository is now, or NULL with *refusal set (NULL when memory ran out). */
static cJSON *copied_entry(const cJSON *entry, const char *api_base, cJSON **refusal) {
    const char *id = text_of(entry, "id");
    cJSON *answer = resolved(id, api_base, refusal);
    const char *name = answer == NULL ? NULL : shown_name(entry, answer);
    const char *full_name = answer == NULL ? NULL : text_of(answer, "fullName");
    cJSON *copy = answer != NULL && check_answer(id, name, full_name, refusal) ? copy_entry(entry, name, full_name) : NULL;
    cJSON_Delete(answer);
    return copy;
}

static cJSON *copied_entries(const cJSON *entries, const char *api_base, cJSON **refusal) {
    cJSON *copied = cJSON_CreateArray();
    for (const cJSON *entry = entries->child; copied != NULL && entry != NULL; entry = entry->next) {
        cJSON *copy = copied_entry(entry, api_base, refusal);
        if (copy == NULL || !check_name_is_free(copied, copy, refusal) || !cJSON_AddItemToArray(copied, copy)) {
            cJSON_Delete(copy);
            cJSON_Delete(copied);
            return NULL;
        }
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

static cJSON *answer_of(cJSON *copy) {
    cJSON *answer = cJSON_CreateObject();
    if (copy == NULL || answer == NULL || !cJSON_AddItemToObject(answer, "copy", copy)) {
        cJSON_Delete(copy);
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
    if (!check_listed_ids(entries, refusal)) {
        return NULL;
    }
    cJSON *copied = copied_entries(entries, request.api_base, refusal);
    return copied == NULL ? NULL : answer_of(copy_of(&request, copied));
}

RUNTIME_EXPORT int32_t runtime_marketplace_build(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, build);
}
