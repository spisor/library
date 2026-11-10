#include "identity_resolve.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fetch_transport.h"
#include "identity.h"
#include "json_service.h"
#include "spisor/json_kind.h"

#define BODY_CAPACITY (256 * 1024)
#define LOOKUP_DEADLINE_MILLISECONDS 30000u
#define LARGEST_EXACT_INTEGER 9007199254740991.0
#define URL_SIZE 1024
#define MESSAGE_SIZE 2048
#define NUMBER_SIZE 32
#define TOKEN_LIMIT 512
#define AUTHORIZATION_SIZE (TOKEN_LIMIT + 16)

/**
 * One repo host kind. Exactly one of fixed_root and host_root_format is set: a host kind with one
 * address, or one whose API root is built from the host the id names, so a new built-in host is one entry.
 * file_route_format is where a file's raw bytes are at the default branch, from the owner/name and the file's
 * path, under file_root, or under the API root when file_root is not set.
 */
typedef struct resolver {
    const char *name;
    const char *fixed_root;
    const char *host_root_format;
    const char *file_root;
    const char *file_route_format;
} resolver;

#define GITEA_FILE_ROUTE "repos/%s/raw/%s"

static const resolver resolvers[] = {
    {"github", "https://api.github.com", NULL, "https://raw.githubusercontent.com", "%s/HEAD/%s"},
    {"gitea", NULL, "https://%.*s/api/v1", NULL, GITEA_FILE_ROUTE},
    {"forgejo", NULL, "https://%.*s/api/v1", NULL, GITEA_FILE_ROUTE},
};

typedef struct lookup_failure {
    long status;
    cJSON *refusal;
} lookup_failure;

static cJSON *refuse(cJSON **refusal, const char *fix, const char *format, ...) {
    char message[MESSAGE_SIZE];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof message, format, arguments);
    va_end(arguments);
    *refusal = runtime_refusal(message, fix);
    return NULL;
}

static int is_text(const char *text, runtime_identity_part part) {
    return strlen(text) == part.length && strncmp(text, part.start, part.length) == 0;
}

static char *copy_part(runtime_identity_part part) {
    char *text = malloc(part.length + 1);
    if (text != NULL) {
        memcpy(text, part.start, part.length);
        text[part.length] = '\0';
    }
    return text;
}

static const resolver *resolver_named(runtime_identity_part name) {
    for (size_t index = 0; index < sizeof resolvers / sizeof resolvers[0]; index++) {
        if (is_text(resolvers[index].name, name)) {
            return &resolvers[index];
        }
    }
    return NULL;
}

static cJSON *repository_answer(const char *name, const char *full_name, const char *default_branch) {
    cJSON *answer = cJSON_CreateObject();
    if (answer == NULL || cJSON_AddStringToObject(answer, "name", name) == NULL ||
        cJSON_AddStringToObject(answer, "fullName", full_name) == NULL ||
        (default_branch != NULL && cJSON_AddStringToObject(answer, "defaultBranch", default_branch) == NULL)) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static cJSON *resolve_local(const runtime_identity *parsed) {
    char *name = copy_part(parsed->path);
    cJSON *answer = name == NULL ? NULL : repository_answer(name, name, NULL);
    free(name);
    return answer;
}

static int build_root(const resolver *kind, const runtime_identity *parsed, const char *override, char *root) {
    if (override != NULL) {
        return snprintf(root, URL_SIZE / 2, "%s", override) < URL_SIZE / 2;
    }
    if (kind->fixed_root != NULL) {
        snprintf(root, URL_SIZE / 2, "%s", kind->fixed_root);
        return 1;
    }
    return snprintf(root, URL_SIZE / 2, kind->host_root_format, (int)parsed->host.length, parsed->host.start) <
           URL_SIZE / 2;
}

static void strip_trailing_slashes(char *root) {
    size_t length = strlen(root);
    while (length > 0 && root[length - 1] == '/') {
        root[--length] = '\0';
    }
}

static int is_name_character(char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '.' || character == '_' || character == '-';
}

static int is_name_segment(const char *start, size_t length) {
    if (length == 0 || (length == 1 && start[0] == '.') || (length == 2 && start[0] == '.' && start[1] == '.')) {
        return 0;
    }
    for (size_t index = 0; index < length; index++) {
        if (!is_name_character(start[index])) {
            return 0;
        }
    }
    return 1;
}

static int is_owner_and_name(const char *text) {
    const char *slash = strchr(text, '/');
    return slash != NULL && is_name_segment(text, (size_t)(slash - text)) &&
           is_name_segment(slash + 1, strlen(slash + 1));
}

static int is_rate_limited(long status) {
    return status == 403 || status == 429;
}

static void fail_status(lookup_failure *failure, const runtime_fetch_answer *answer, const char *url) {
    if (answer->outcome == RUNTIME_FETCH_STATUS) {
        failure->status = answer->status;
    } else if (answer->outcome == RUNTIME_FETCH_REFUSED) {
        refuse(&failure->refusal, answer->fix, "%s", answer->detail);
    } else if (answer->outcome == RUNTIME_FETCH_STOPPED_BY_SINK) {
        refuse(&failure->refusal, "check that the host is the repo host the id names", "%s answered more than a lookup reads",
               url);
    } else {
        refuse(&failure->refusal, RUNTIME_FETCH_NETWORK_FIX, "%s ended before its whole answer arrived", url);
    }
}

static cJSON *parse_body(const uint8_t *body, size_t length, const char *url, cJSON **refusal) {
    cJSON *repository = spisor_parse_json(body, length);
    if (!cJSON_IsObject(repository)) {
        cJSON_Delete(repository);
        return refuse(refusal, "check that the host is the repo host the id names", "%s did not answer a repository", url);
    }
    return repository;
}

/** A repository answer when the host gives one, sending token when it is not NULL; NULL with failure->status set for another HTTP status, or failure->refusal set. */
static cJSON *lookup(const char *url, const char *token, lookup_failure *failure) {
    char authorization[AUTHORIZATION_SIZE];
    if (token != NULL) {
        snprintf(authorization, sizeof authorization, "token %s", token);
    }
    uint8_t *body = malloc(BODY_CAPACITY);
    if (body == NULL) {
        return NULL;
    }
    runtime_fetch_sink sink;
    runtime_fetch_sink_start_memory(&sink, body, BODY_CAPACITY);
    runtime_fetch_answer answer;
    memset(&answer, 0, sizeof answer);
    runtime_fetch_into_memory(url, token == NULL ? NULL : authorization, LOOKUP_DEADLINE_MILLISECONDS, &sink, &answer);
    cJSON *repository = NULL;
    if (answer.outcome == RUNTIME_FETCH_RECEIVED) {
        repository = parse_body(body, (size_t)sink.bytes, url, &failure->refusal);
    } else {
        fail_status(failure, &answer, url);
    }
    free(answer.etag);
    free(answer.last_modified);
    free(body);
    return repository;
}

/** The repository's number as digits, or 0 when it is absent, fractional or too large for a double to hold exactly. */
static int read_number(const cJSON *repository, char *digits) {
    const cJSON *number = cJSON_GetObjectItemCaseSensitive(repository, "id");
    if (!cJSON_IsNumber(number) || number->valuedouble < 0 || number->valuedouble > LARGEST_EXACT_INTEGER ||
        floor(number->valuedouble) != number->valuedouble) {
        return 0;
    }
    snprintf(digits, NUMBER_SIZE, "%.0f", number->valuedouble);
    return 1;
}

static const char *text_member(const cJSON *repository, const char *member) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(repository, member);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static cJSON *answer_from(const cJSON *repository, const char *url, cJSON **refusal) {
    const char *name = text_member(repository, "name");
    const char *full_name = text_member(repository, "full_name");
    if (name == NULL || full_name == NULL) {
        return refuse(refusal, "check that the host is the repo host the id names",
                      "%s answered a repository with no name or full_name", url);
    }
    return repository_answer(name, full_name, text_member(repository, "default_branch"));
}

static cJSON *refuse_unreadable_number(const char *url, cJSON **refusal) {
    return refuse(refusal, "check that the host is the repo host the id names",
                  "%s answered no repository number that can be read exactly", url);
}

static cJSON *confirm_by_number(const cJSON *repository, const char *asked, const char *url, cJSON **refusal) {
    char found[NUMBER_SIZE];
    if (!read_number(repository, found)) {
        return refuse_unreadable_number(url, refusal);
    }
    if (strcmp(found, asked) != 0) {
        return refuse(refusal, "check the number in the id", "%s answered repository %s when repository %s was asked for", url,
                      found, asked);
    }
    return answer_from(repository, url, refusal);
}

static cJSON *confirm_last_known_name(const cJSON *repository, const char *asked, const char *last_known,
                                      const char *url, cJSON **refusal) {
    char found[NUMBER_SIZE];
    if (!read_number(repository, found)) {
        return refuse_unreadable_number(url, refusal);
    }
    if (strcmp(found, asked) != 0) {
        char fix[MESSAGE_SIZE];
        snprintf(fix, sizeof fix,
                 "never install from %s: it is repository %s now, not repository %s; ask the owner of repository %s for its current name",
                 last_known, found, asked, asked);
        return refuse(refusal, fix, "the name %s now belongs to repository %s, not repository %s", last_known, found, asked);
    }
    return answer_from(repository, url, refusal);
}

static cJSON *refuse_rate_limited(const char *url, long status, cJSON **refusal) {
    return refuse(refusal, "wait for the host's rate limit to reset, then try again",
                  "%s answered HTTP %ld: the host is rate limiting this caller", url, status);
}

static cJSON *refuse_unexpected_status(const char *url, long status, cJSON **refusal) {
    return refuse(refusal, "check that the host is the repo host the id names", "%s answered HTTP %ld", url, status);
}

static int url_for(const char *root, const char *route, const char *subject, char *url) {
    return snprintf(url, URL_SIZE, "%s/%s/%s", root, route, subject) < URL_SIZE;
}

static cJSON *refuse_too_long(cJSON **refusal) {
    return refuse(refusal, "use a shorter apiBase or lastKnown", "the lookup address is too long");
}

static cJSON *settle_name_lookup(const char *url, const lookup_failure *failure, const char *asked,
                                 const char *last_known, cJSON **refusal) {
    if (failure->status == 404) {
        return refuse(refusal, "check the id, and that the repository is public",
                      "repository %s and its last known name %s were not found, or the repository is private", asked,
                      last_known);
    }
    return is_rate_limited(failure->status) ? refuse_rate_limited(url, failure->status, refusal)
                                            : refuse_unexpected_status(url, failure->status, refusal);
}

static cJSON *resolve_by_last_known_name(const char *root, const char *asked, const char *last_known, const char *token,
                                         cJSON **refusal) {
    char url[URL_SIZE];
    if (!url_for(root, "repos", last_known, url)) {
        return refuse_too_long(refusal);
    }
    lookup_failure failure = {0, NULL};
    cJSON *repository = lookup(url, token, &failure);
    cJSON *answer = NULL;
    if (repository != NULL) {
        answer = confirm_last_known_name(repository, asked, last_known, url, refusal);
    } else if (failure.refusal != NULL) {
        *refusal = failure.refusal;
    } else if (failure.status != 0) {
        answer = settle_name_lookup(url, &failure, asked, last_known, refusal);
    }
    cJSON_Delete(repository);
    return answer;
}

static int cannot_be_asked_by_number(long status) {
    return status == 404 || status == 401;
}

static cJSON *settle_number_lookup(const char *url, const lookup_failure *failure, const char *asked, cJSON **refusal) {
    if (is_rate_limited(failure->status)) {
        return refuse_rate_limited(url, failure->status, refusal);
    }
    if (failure->status == 404) {
        return refuse(refusal, "check the id, and that the repository is public",
                      "repository %s was not found or is private", asked);
    }
    if (failure->status == 401) {
        return refuse(refusal, "give the repository's last known owner/name so its number can be confirmed from it",
                      "%s wants a token to look a repository up by number", url);
    }
    return refuse_unexpected_status(url, failure->status, refusal);
}

static cJSON *resolve_by_number(const char *root, const char *asked, const char *last_known, const char *token,
                                cJSON **refusal) {
    char url[URL_SIZE];
    if (!url_for(root, "repositories", asked, url)) {
        return refuse_too_long(refusal);
    }
    lookup_failure failure = {0, NULL};
    cJSON *repository = lookup(url, token, &failure);
    cJSON *answer = NULL;
    if (repository != NULL) {
        answer = confirm_by_number(repository, asked, url, refusal);
    } else if (failure.refusal != NULL) {
        *refusal = failure.refusal;
    } else if (last_known != NULL && cannot_be_asked_by_number(failure.status)) {
        answer = resolve_by_last_known_name(root, asked, last_known, token, refusal);
    } else if (failure.status != 0) {
        answer = settle_number_lookup(url, &failure, asked, refusal);
    }
    cJSON_Delete(repository);
    return answer;
}

static cJSON *refuse_host_shape(const resolver *kind, const runtime_identity *parsed, const char *id, cJSON **refusal) {
    if (kind->fixed_root != NULL) {
        return refuse(refusal, "write the id without a host", "the id %s names a host, which %s does not take", id, kind->name);
    }
    char number[NUMBER_SIZE];
    snprintf(number, sizeof number, "%.*s", (int)parsed->number.length, parsed->number.start);
    char fix[MESSAGE_SIZE];
    snprintf(fix, sizeof fix, "write it as %s@<host>:%s", kind->name, number);
    return refuse(refusal, fix, "the id %s names no host", id);
}

static int host_shape_fits(const resolver *kind, const runtime_identity *parsed) {
    return (kind->fixed_root != NULL) == (parsed->host.start == NULL);
}

static int is_token(const cJSON *token) {
    if (!cJSON_IsString(token) || token->valuestring[0] == '\0' || strlen(token->valuestring) > TOKEN_LIMIT) {
        return 0;
    }
    for (const unsigned char *at = (const unsigned char *)token->valuestring; *at != '\0'; at++) {
        if (*at <= 0x20 || *at >= 0x7F) {
            return 0;
        }
    }
    return 1;
}

static cJSON *resolve_remote(const resolver *kind, const runtime_identity *parsed, const char *id,
                             const cJSON *argument, cJSON **refusal) {
    const cJSON *api_base = cJSON_GetObjectItemCaseSensitive(argument, "apiBase");
    const cJSON *last_known = cJSON_GetObjectItemCaseSensitive(argument, "lastKnown");
    const cJSON *token = cJSON_GetObjectItemCaseSensitive(argument, "token");
    if (token != NULL && !is_token(token)) {
        return refuse(refusal, "send token as the access token itself, on one line with no spaces",
                      "the token is not usable");
    }
    if (api_base != NULL && !runtime_fetch_plain_http_is_allowed()) {
        return refuse(refusal, "leave apiBase out: the host's own API address is used",
                      "apiBase is only honoured by the test build");
    }
    if (!host_shape_fits(kind, parsed)) {
        return refuse_host_shape(kind, parsed, id, refusal);
    }
    if ((last_known != NULL && (!cJSON_IsString(last_known) || !is_owner_and_name(last_known->valuestring))) ||
        (api_base != NULL && (!cJSON_IsString(api_base) || api_base->valuestring[0] == '\0'))) {
        return refuse(refusal, "send lastKnown as owner/name and apiBase as a non-empty address", "lastKnown or apiBase is not usable");
    }
    char root[URL_SIZE / 2];
    if (!build_root(kind, parsed, api_base == NULL ? NULL : api_base->valuestring, root)) {
        return refuse_too_long(refusal);
    }
    strip_trailing_slashes(root);
    char *asked = copy_part(parsed->number);
    cJSON *answer = asked == NULL ? NULL
                                  : resolve_by_number(root, asked, last_known == NULL ? NULL : last_known->valuestring,
                                                    token == NULL ? NULL : token->valuestring, refusal);
    free(asked);
    return answer;
}

static cJSON *resolve_unknown(runtime_identity_part name, cJSON **refusal) {
    char fix[MESSAGE_SIZE];
    snprintf(fix, sizeof fix, "install a plugin that provides a resolver for %.*s", (int)name.length, name.start);
    return refuse(refusal, fix, "no resolver is built in for %.*s", (int)name.length, name.start);
}

static cJSON *resolve(const cJSON *argument, cJSON **refusal) {
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(argument, "id");
    if (!cJSON_IsString(id)) {
        return refuse(refusal, "send {\"id\": \"<id>\"}", "the argument carries no id");
    }
    runtime_identity parsed;
    if (!runtime_identity_parse_id(id->valuestring, &parsed)) {
        return refuse(refusal, "send an id such as github:<number> or local:<name>", "the id is not valid");
    }
    if (is_text("local", parsed.resolver)) {
        return resolve_local(&parsed);
    }
    const resolver *kind = resolver_named(parsed.resolver);
    return kind == NULL ? resolve_unknown(parsed.resolver, refusal)
                        : resolve_remote(kind, &parsed, id->valuestring, argument, refusal);
}

static cJSON *repository_argument(const char *id, const char *api_base) {
    cJSON *argument = cJSON_CreateObject();
    if (argument == NULL || cJSON_AddStringToObject(argument, "id", id) == NULL ||
        (api_base != NULL && cJSON_AddStringToObject(argument, "apiBase", api_base) == NULL)) {
        cJSON_Delete(argument);
        return NULL;
    }
    return argument;
}

static int file_url(const resolver *kind, const runtime_identity *parsed, const char *api_base, const char *full_name,
                    const char *path, char *url) {
    char root[URL_SIZE / 2];
    char route[URL_SIZE / 2];
    int root_fits = api_base == NULL && kind->file_root != NULL ? snprintf(root, sizeof root, "%s", kind->file_root) < (int)sizeof root
                                                                : build_root(kind, parsed, api_base, root);
    if (!root_fits || snprintf(route, sizeof route, kind->file_route_format, full_name, path) >= (int)sizeof route) {
        return 0;
    }
    strip_trailing_slashes(root);
    return snprintf(url, URL_SIZE, "%s/%s", root, route) < URL_SIZE;
}

static runtime_repo_file_status fetch_file(const char *url, uint8_t **content, size_t *length) {
    uint8_t *body = malloc(BODY_CAPACITY);
    if (body == NULL) {
        return RUNTIME_REPO_FILE_OUT_OF_MEMORY;
    }
    runtime_fetch_sink sink;
    runtime_fetch_sink_start_memory(&sink, body, BODY_CAPACITY);
    runtime_fetch_answer answer;
    memset(&answer, 0, sizeof answer);
    runtime_fetch_into_memory(url, NULL, LOOKUP_DEADLINE_MILLISECONDS, &sink, &answer);
    runtime_repo_file_status status = RUNTIME_REPO_FILE_UNAVAILABLE;
    if (answer.outcome == RUNTIME_FETCH_RECEIVED) {
        status = RUNTIME_REPO_FILE_READ;
    } else if (answer.outcome == RUNTIME_FETCH_STATUS && answer.status == 404) {
        status = RUNTIME_REPO_FILE_ABSENT;
    } else if (answer.outcome == RUNTIME_FETCH_STOPPED_BY_SINK) {
        status = RUNTIME_REPO_FILE_TOO_LARGE;
    }
    free(answer.etag);
    free(answer.last_modified);
    if (status == RUNTIME_REPO_FILE_READ) {
        *content = body;
        *length = (size_t)sink.bytes;
    } else {
        free(body);
    }
    return status;
}

static runtime_repo_file_status read_from_repository(const char *id, const char *api_base, const cJSON *repository,
                                                     const char *path, uint8_t **content, size_t *length) {
    runtime_identity parsed;
    const char *full_name = text_member(repository, "fullName");
    const resolver *kind = runtime_identity_parse_id(id, &parsed) ? resolver_named(parsed.resolver) : NULL;
    char url[URL_SIZE];
    if (kind == NULL || full_name == NULL || !is_owner_and_name(full_name) ||
        !file_url(kind, &parsed, api_base, full_name, path, url)) {
        return RUNTIME_REPO_FILE_UNAVAILABLE;
    }
    return fetch_file(url, content, length);
}

int runtime_identity_repo_url(const char *id, const char *api_base, const char *full_name, const char *route, char *url,
                              size_t size) {
    runtime_identity parsed;
    const resolver *kind = runtime_identity_parse_id(id, &parsed) ? resolver_named(parsed.resolver) : NULL;
    char root[URL_SIZE / 2];
    if (kind == NULL || !is_owner_and_name(full_name) || !build_root(kind, &parsed, api_base, root)) {
        return 0;
    }
    strip_trailing_slashes(root);
    return snprintf(url, size, "%s/repos/%s/%s", root, full_name, route) < (int)size;
}

static cJSON *resolve_repository(const char *id, const char *api_base, cJSON **refusal) {
    cJSON *argument = repository_argument(id, api_base);
    cJSON *repository = argument == NULL ? NULL : resolve(argument, refusal);
    cJSON_Delete(argument);
    return repository;
}

static int still_names_the_same_repository(const char *id, const char *api_base, const cJSON *before) {
    cJSON *refusal = NULL;
    cJSON *after = resolve_repository(id, api_base, &refusal);
    const char *before_name = text_member(before, "fullName");
    const char *after_name = after == NULL ? NULL : text_member(after, "fullName");
    int same = before_name != NULL && after_name != NULL && strcmp(before_name, after_name) == 0;
    cJSON_Delete(after);
    cJSON_Delete(refusal);
    return same;
}

runtime_repo_file_status runtime_identity_read_file(const char *id, const char *api_base, const char *path,
                                                    uint8_t **content, size_t *length, cJSON **refusal) {
    cJSON *repository = resolve_repository(id, api_base, refusal);
    if (repository == NULL) {
        return *refusal == NULL ? RUNTIME_REPO_FILE_OUT_OF_MEMORY : RUNTIME_REPO_FILE_UNAVAILABLE;
    }
    runtime_repo_file_status status = read_from_repository(id, api_base, repository, path, content, length);
    if (status == RUNTIME_REPO_FILE_READ && !still_names_the_same_repository(id, api_base, repository)) {
        free(*content);
        *content = NULL;
        status = RUNTIME_REPO_FILE_UNAVAILABLE;
    }
    cJSON_Delete(repository);
    return status;
}

RUNTIME_EXPORT int32_t runtime_identity_resolve(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, resolve);
}
