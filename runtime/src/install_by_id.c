#include "install_by_id.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache_deploy.h"
#include "cache_install.h"
#include "catalog_read.h"
#include "fetch_transport.h"
#include "identity.h"
#include "identity_also_at.h"
#include "identity_resolve.h"
#include "json_service.h"
#include "platform_key.h"
#include "sha256.h"
#include "spisor/json_kind.h"

#define INDEX_ASSET "weivel-index.json"
#define RELEASE_CAPACITY (256 * 1024)
#define INDEX_CAPACITY (4 * 1024 * 1024)
#define LOOKUP_DEADLINE_MILLISECONDS 30000u
#define URL_SIZE 1024
#define MESSAGE_SIZE 2048

typedef struct install_request {
    const char *id;
    const char *cache;
    const char *plugin_directory;
    const char *version;
    const char *last_known;
    const char *api_base;
    const char *recorded_primary;
} install_request;

typedef struct release_index {
    cJSON *catalog;
    const cJSON *item;
    char sha256[RUNTIME_SHA256_HEX_SIZE];
} release_index;

static cJSON *refuse(cJSON **refusal, const char *fix, const char *format, ...) {
    char message[MESSAGE_SIZE];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof message, format, arguments);
    va_end(arguments);
    *refusal = runtime_refusal(message, fix);
    return NULL;
}

static const char *text_of(const cJSON *object, const char *member) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, member);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

/** Reads an optional text member into *value, NULL when absent; 0 with a refusal when it is present and not usable text. */
static int read_optional_text(const cJSON *argument, const char *member, const char **value, cJSON **refusal) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(argument, member);
    *value = cJSON_IsString(item) && item->valuestring[0] != '\0' ? item->valuestring : NULL;
    if (item != NULL && *value == NULL) {
        refuse(refusal, "send each text member as a non-empty string, or leave it out", "%s is not usable text", member);
        return 0;
    }
    return 1;
}

static int read_required_text(const cJSON *argument, const char *member, const char **value, cJSON **refusal) {
    if (!read_optional_text(argument, member, value, refusal)) {
        return 0;
    }
    if (*value == NULL) {
        refuse(refusal, "send {id, cache, pluginDir} and, when they are known, version, lastKnown and apiBase",
               "the argument carries no %s", member);
        return 0;
    }
    return 1;
}

static int read_request(const cJSON *argument, install_request *request, cJSON **refusal) {
    memset(request, 0, sizeof *request);
    runtime_identity parsed;
    if (!read_required_text(argument, "id", &request->id, refusal) ||
        !read_required_text(argument, "cache", &request->cache, refusal) ||
        !read_required_text(argument, "pluginDir", &request->plugin_directory, refusal) ||
        !read_optional_text(argument, "version", &request->version, refusal) ||
        !read_optional_text(argument, "lastKnown", &request->last_known, refusal) ||
        !read_optional_text(argument, "apiBase", &request->api_base, refusal) ||
        !read_optional_text(argument, "recordedPrimary", &request->recorded_primary, refusal)) {
        return 0;
    }
    if (!runtime_identity_parse_id(request->id, &parsed)) {
        refuse(refusal, "send an id such as github:<number>", "the id is not valid");
        return 0;
    }
    if (parsed.number.start == NULL) {
        refuse(refusal, "a local plugin is deployed from its folder, not installed", "%s names no repo host to install from",
               request->id);
        return 0;
    }
    return 1;
}

static void refuse_fetch(const runtime_fetch_answer *answer, const char *url, cJSON **refusal) {
    if (answer->outcome == RUNTIME_FETCH_STATUS && (answer->status == 403 || answer->status == 429)) {
        refuse(refusal, "wait for the host's rate limit to reset, then try again", "%s answered HTTP %ld: an anonymous caller is rate limited by this host",
               url, answer->status);
    } else if (answer->outcome == RUNTIME_FETCH_STATUS) {
        refuse(refusal, "check that the repo has a release that publishes " INDEX_ASSET, "%s answered HTTP %ld", url,
               answer->status);
    } else if (answer->outcome == RUNTIME_FETCH_REFUSED) {
        refuse(refusal, answer->fix, "%s", answer->detail);
    } else if (answer->outcome == RUNTIME_FETCH_STOPPED_BY_SINK) {
        refuse(refusal, "check that the release publishes the files it should", "%s answered more than an install reads", url);
    } else {
        refuse(refusal, RUNTIME_FETCH_NETWORK_FIX, "%s ended before its whole answer arrived", url);
    }
}

/** A file: url is read only when the fetch test hook allows plain http, so only a test can aim an install at a local release. */
static int is_readable_file_url(const char *url) {
    return strncmp(url, "file:", 5) == 0 && runtime_fetch_plain_http_is_allowed();
}

/** The bytes at url, freed with free and NUL terminated, with their length; NULL with a refusal, or without one out of memory. */
static uint8_t *fetch_bytes(const char *url, size_t capacity, size_t *length, cJSON **refusal) {
    uint8_t *body = malloc(capacity + 1);
    if (body == NULL) {
        return NULL;
    }
    runtime_fetch_sink sink;
    runtime_fetch_sink_start_memory(&sink, body, capacity);
    runtime_fetch_answer answer;
    memset(&answer, 0, sizeof answer);
    if (is_readable_file_url(url)) {
        runtime_fetch_file_url(url, &sink, &answer);
    } else {
        runtime_fetch_into_memory(url, LOOKUP_DEADLINE_MILLISECONDS, &sink, &answer);
    }
    int received = answer.outcome == RUNTIME_FETCH_RECEIVED;
    if (received) {
        *length = (size_t)sink.bytes;
        body[*length] = '\0';
    } else {
        refuse_fetch(&answer, url, refusal);
    }
    free(answer.etag);
    free(answer.last_modified);
    if (!received) {
        free(body);
        return NULL;
    }
    return body;
}

static cJSON *resolve_argument(const install_request *request) {
    cJSON *argument = cJSON_CreateObject();
    int built = argument != NULL && cJSON_AddStringToObject(argument, "id", request->id) != NULL &&
                (request->last_known == NULL || cJSON_AddStringToObject(argument, "lastKnown", request->last_known) != NULL) &&
                (request->api_base == NULL || cJSON_AddStringToObject(argument, "apiBase", request->api_base) != NULL);
    if (!built) {
        cJSON_Delete(argument);
        argument = NULL;
    }
    return argument;
}

static cJSON *resolve_repository(const install_request *request, cJSON **refusal) {
    return runtime_call_or_refuse(runtime_identity_resolve, resolve_argument(request), refusal);
}

static cJSON *unconfirmed_set(const char *id) {
    cJSON *set = cJSON_CreateObject();
    cJSON *ids = set == NULL ? NULL : cJSON_AddArrayToObject(set, "ids");
    int built = ids != NULL && cJSON_AddItemToArray(ids, cJSON_CreateString(id)) &&
                cJSON_AddStringToObject(set, "primary", id) != NULL &&
                cJSON_AddFalseToObject(set, "primaryConfirmed") != NULL;
    if (!built) {
        cJSON_Delete(set);
        set = NULL;
    }
    return set;
}

static int has_confirmed_primary(const cJSON *set) {
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(set, "primaryConfirmed")) &&
           !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(set, "startFileUnavailable"));
}

static cJSON *also_at_argument(const install_request *request) {
    cJSON *argument = cJSON_CreateObject();
    int built = argument != NULL && cJSON_AddStringToObject(argument, "id", request->id) != NULL &&
                (request->api_base == NULL || cJSON_AddStringToObject(argument, "apiBase", request->api_base) != NULL);
    if (!built) {
        cJSON_Delete(argument);
        argument = NULL;
    }
    return argument;
}

/** The set the repo's copies agree on; the id alone, unconfirmed, when that cannot be read or confirmed. NULL when memory runs out. */
static cJSON *verified_set(const install_request *request) {
    cJSON *refusal = NULL;
    cJSON *found = runtime_call_or_refuse(runtime_identity_also_at, also_at_argument(request), &refusal);
    int answered = found != NULL || refusal != NULL;
    cJSON_Delete(refusal);
    if (found != NULL && has_confirmed_primary(found)) {
        return found;
    }
    cJSON_Delete(found);
    return answered ? unconfirmed_set(request->id) : NULL;
}

static int needs_user_acceptance(const install_request *request, const cJSON *set, int *needed, cJSON **refusal) {
    *needed = 0;
    if (request->recorded_primary == NULL) {
        return 1;
    }
    cJSON *argument = cJSON_CreateObject();
    int built = argument != NULL && cJSON_AddStringToObject(argument, "recorded", request->recorded_primary) != NULL &&
                cJSON_AddStringToObject(argument, "primary", text_of(set, "primary")) != NULL;
    cJSON *decision = built ? runtime_call_or_refuse(runtime_identity_accepts_version, argument, refusal) : NULL;
    if (!built) {
        cJSON_Delete(argument);
    }
    *needed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(decision, "needsUserAcceptance"));
    int decided = decision != NULL;
    cJSON_Delete(decision);
    return decided;
}

static const char *index_asset_url(const cJSON *release) {
    const cJSON *asset;
    cJSON_ArrayForEach(asset, cJSON_GetObjectItemCaseSensitive(release, "assets")) {
        const char *name = text_of(asset, "name");
        const char *url = text_of(asset, "browser_download_url");
        if (name != NULL && url != NULL && strcmp(name, INDEX_ASSET) == 0) {
            return url;
        }
    }
    return NULL;
}

static int same_repository_as(const cJSON *before, const cJSON *after) {
    const char *before_name = text_of(before, "fullName");
    const char *after_name = text_of(after, "fullName");
    return before_name != NULL && after_name != NULL && strcmp(before_name, after_name) == 0;
}

/** The index url of the latest release, freed with free; NULL with a refusal or, out of memory, without one. */
static char *find_index_url(const install_request *request, const cJSON *repository, cJSON **refusal) {
    char url[URL_SIZE];
    const char *full_name = text_of(repository, "fullName");
    if (full_name == NULL ||
        !runtime_identity_repo_url(request->id, request->api_base, full_name, "releases/latest", url, sizeof url)) {
        refuse(refusal, "use a shorter apiBase", "the address of the latest release of %s is not usable", request->id);
        return NULL;
    }
    size_t length = 0;
    uint8_t *bytes = fetch_bytes(url, RELEASE_CAPACITY, &length, refusal);
    int fetched = bytes != NULL;
    cJSON *release = fetched ? spisor_parse_json(bytes, length) : NULL;
    free(bytes);
    const char *asset_url = release == NULL ? NULL : index_asset_url(release);
    if (fetched && asset_url == NULL) {
        refuse(refusal, "ask the owner to publish " INDEX_ASSET " with each release", "the latest release of %s has no %s",
               full_name, INDEX_ASSET);
    }
    char *copy = asset_url == NULL ? NULL : malloc(strlen(asset_url) + 1);
    if (copy != NULL) {
        strcpy(copy, asset_url);
    }
    cJSON_Delete(release);
    return copy;
}

static cJSON *catalog_around(const char *id, const cJSON *item) {
    cJSON *catalog = cJSON_CreateObject();
    int formatted = catalog != NULL && cJSON_AddNumberToObject(catalog, "format", 1) != NULL;
    cJSON *items = formatted ? cJSON_AddObjectToObject(catalog, "items") : NULL;
    cJSON *copy = items == NULL ? NULL : cJSON_Duplicate(item, 1);
    if (copy == NULL || !cJSON_AddItemToObject(items, id, copy)) {
        cJSON_Delete(copy);
        cJSON_Delete(catalog);
        catalog = NULL;
    }
    return catalog;
}

static void refuse_catalog(runtime_catalog_refusal *catalog_refusal, const char *url, cJSON **refusal) {
    char *message = runtime_catalog_refusal_message(catalog_refusal);
    refuse(refusal, catalog_refusal->fix == NULL ? "correct the release index" : catalog_refusal->fix, "%s is not a usable release index: %s",
           url, message == NULL ? "it is refused" : message);
    free(message);
    runtime_catalog_free_refusal(catalog_refusal);
}

/** The index's one item, checked as a catalog item with its file urls resolved against the index's own url. */
static cJSON *read_catalog(const char *id, const char *url, const uint8_t *bytes, size_t length, cJSON **refusal) {
    cJSON *item = spisor_parse_json(bytes, length);
    cJSON *wrapped = cJSON_IsObject(item) ? catalog_around(id, item) : NULL;
    char *text = wrapped == NULL ? NULL : cJSON_PrintUnformatted(wrapped);
    runtime_catalog_refusal catalog_refusal;
    cJSON *catalog = text == NULL ? NULL : runtime_catalog_read_text(text, strlen(text), url, &catalog_refusal);
    if (text != NULL && catalog == NULL) {
        refuse_catalog(&catalog_refusal, url, refusal);
    } else if (text == NULL && item != NULL && !cJSON_IsObject(item)) {
        refuse(refusal, "publish the catalog item as a JSON object", "%s is not a JSON object", url);
    } else if (text == NULL && item == NULL) {
        refuse(refusal, "publish the catalog item as JSON", "%s is not JSON", url);
    }
    cJSON_free(text);
    cJSON_Delete(wrapped);
    cJSON_Delete(item);
    return catalog;
}

static void digest_of(const uint8_t *bytes, size_t length, char sha256[RUNTIME_SHA256_HEX_SIZE]) {
    runtime_sha256 hash;
    runtime_sha256_start(&hash);
    runtime_sha256_add(&hash, bytes, length);
    runtime_sha256_finish_hex(&hash, sha256);
}

static int is_still_the_repository(const install_request *request, const cJSON *repository, cJSON **refusal) {
    cJSON *again = resolve_repository(request, refusal);
    int same = again != NULL && same_repository_as(repository, again);
    if (again != NULL && !same) {
        refuse(refusal, "try the install again; if it keeps happening, check who owns the repo now",
               "%s was %s when the install began and is %s now", request->id, text_of(repository, "fullName"),
               text_of(again, "fullName"));
    }
    cJSON_Delete(again);
    return same;
}

static int is_plugin_item(const cJSON *item, cJSON **refusal) {
    const char *kind = text_of(item, "kind");
    if (kind != NULL && strcmp(kind, "plugin") == 0) {
        return 1;
    }
    refuse(refusal, "install.byId installs plugins", "the release index describes an item of kind %s", kind == NULL ? "(none)" : kind);
    return 0;
}

static int read_release_index(const install_request *request, const cJSON *repository, release_index *index,
                              cJSON **refusal) {
    memset(index, 0, sizeof *index);
    char *url = find_index_url(request, repository, refusal);
    size_t length = 0;
    uint8_t *bytes = url == NULL ? NULL : fetch_bytes(url, INDEX_CAPACITY, &length, refusal);
    int tied = bytes != NULL && is_still_the_repository(request, repository, refusal);
    index->catalog = tied ? read_catalog(request->id, url, bytes, length, refusal) : NULL;
    index->item = index->catalog == NULL
                      ? NULL
                      : cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(index->catalog, "items"), request->id);
    if (bytes != NULL) {
        digest_of(bytes, length, index->sha256);
    }
    free(bytes);
    free(url);
    return index->item != NULL && is_plugin_item(index->item, refusal);
}

static cJSON *deploy_argument(const char *directory, const char *plugin_directory, const char *id, const char *name) {
    cJSON *argument = cJSON_CreateObject();
    cJSON *identity = argument == NULL ? NULL : cJSON_AddObjectToObject(argument, "identity");
    int built = identity != NULL && cJSON_AddStringToObject(identity, "id", id) != NULL &&
                cJSON_AddStringToObject(identity, "name", name) != NULL &&
                cJSON_AddStringToObject(argument, "directory", directory) != NULL &&
                cJSON_AddStringToObject(argument, "pluginDir", plugin_directory) != NULL;
    if (!built) {
        cJSON_Delete(argument);
        argument = NULL;
    }
    return argument;
}

static const char *chosen_version(const install_request *request, const release_index *index, cJSON **refusal) {
    const char *version = request->version != NULL ? request->version : text_of(index->item, "latest");
    if (version == NULL) {
        refuse(refusal, "ask the owner to name a latest version in the release index", "the release index names no latest version");
    }
    return version;
}

/** Installs the chosen version and deploys it, answering the version; NULL with a refusal, or without one out of memory. */
static const char *install_and_deploy(const install_request *request, const cJSON *repository, const release_index *index,
                                      cJSON **refusal) {
    const char *version = chosen_version(request, index, refusal);
    cJSON *installed = version == NULL ? NULL
                                       : runtime_cache_install_version(request->cache, request->id, index->item, version,
                                                                       runtime_platform_key(), refusal);
    const char *directory = text_of(installed, "directory");
    cJSON *deployed = directory == NULL ? NULL
                                        : runtime_call_or_refuse(runtime_cache_deploy,
                                                                 deploy_argument(directory, request->plugin_directory, request->id,
                                                                                 text_of(repository, "name")),
                                                                 refusal);
    cJSON_Delete(installed);
    int succeeded = deployed != NULL;
    cJSON_Delete(deployed);
    return succeeded ? version : NULL;
}

static int add_text(cJSON *answer, const char *member, const char *value) {
    return cJSON_AddStringToObject(answer, member, value == NULL ? "" : value) != NULL;
}

static cJSON *set_answer(const install_request *request, const cJSON *repository, const cJSON *set) {
    cJSON *answer = cJSON_CreateObject();
    int built = answer != NULL && add_text(answer, "id", request->id) && add_text(answer, "name", text_of(repository, "name")) &&
                add_text(answer, "fullName", text_of(repository, "fullName")) &&
                cJSON_AddItemToObject(answer, "ids", cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(set, "ids"), 1)) &&
                add_text(answer, "primary", text_of(set, "primary")) &&
                cJSON_AddBoolToObject(answer, "primaryConfirmed", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(set, "primaryConfirmed"))) != NULL;
    if (!built) {
        cJSON_Delete(answer);
        answer = NULL;
    }
    return answer;
}

static cJSON *acceptance_answer(const install_request *request, const cJSON *repository, const cJSON *set) {
    cJSON *answer = set_answer(request, repository, set);
    if (answer != NULL && (!add_text(answer, "recordedPrimary", request->recorded_primary) ||
                           cJSON_AddTrueToObject(answer, "needsUserAcceptance") == NULL)) {
        cJSON_Delete(answer);
        answer = NULL;
    }
    return answer;
}

static const char *commit_of(const cJSON *item, const char *version) {
    return text_of(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(item, "versions"), version), "commit");
}

static cJSON *installed_answer(const install_request *request, const cJSON *repository, const cJSON *set,
                               const release_index *index, const char *version) {
    cJSON *answer = set_answer(request, repository, set);
    if (answer != NULL && (!add_text(answer, "version", version) || !add_text(answer, "commit", commit_of(index->item, version)) ||
                           !add_text(answer, "sha256", index->sha256))) {
        cJSON_Delete(answer);
        answer = NULL;
    }
    return answer;
}

static cJSON *install_verified(const install_request *request, const cJSON *repository, const cJSON *set,
                               cJSON **refusal) {
    int needed = 0;
    if (!needs_user_acceptance(request, set, &needed, refusal)) {
        return NULL;
    }
    if (needed) {
        return acceptance_answer(request, repository, set);
    }
    release_index index;
    cJSON *answer = NULL;
    if (read_release_index(request, repository, &index, refusal)) {
        const char *version = install_and_deploy(request, repository, &index, refusal);
        answer = version == NULL ? NULL : installed_answer(request, repository, set, &index, version);
    }
    cJSON_Delete(index.catalog);
    return answer;
}

static cJSON *install_by_id(const cJSON *argument, cJSON **refusal) {
    install_request request;
    if (!read_request(argument, &request, refusal)) {
        return NULL;
    }
    cJSON *repository = resolve_repository(&request, refusal);
    cJSON *set = repository == NULL ? NULL : verified_set(&request);
    cJSON *answer = set == NULL ? NULL : install_verified(&request, repository, set, refusal);
    cJSON_Delete(set);
    cJSON_Delete(repository);
    return answer;
}

RUNTIME_EXPORT int32_t runtime_install_by_id(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, install_by_id);
}
