#include "install_by_id.h"
#include "allocation.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache_deploy.h"
#include "cache_install.h"
#include "cache_place.h"
#include "catalog.h"
#include "cache_refusal.h"
#include "catalog_read.h"
#include "fetch_transport.h"
#include "identity.h"
#include "identity_also_at.h"
#include "identity_resolve.h"
#include "json_service.h"
#include "node_json_file.h"
#include "platform_key.h"
#include "runtime/identity.h"
#include "sha256.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"

#define INDEX_ASSET "weivel-index.json"
#define RELEASE_CAPACITY (256 * 1024)
#define INDEX_CAPACITY (4 * 1024 * 1024)
#define LOOKUP_DEADLINE_MILLISECONDS 30000u
#define URL_SIZE 1024
#define MESSAGE_SIZE 2048

typedef struct install_request {
    const char *id;
    const char *repo_id;
    const char *replaces;
    const char *cache;
    const char *plugin_directory;
    const char *version;
    const char *last_known;
    const char *api_base;
    const char *accept_primary;
    char *recorded_primary;
    runtime_install_by_id_taker take;
    void *taken;
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

/** 1 for a text that is NULL or not empty; 0 with a refusal naming member for an empty one. */
static int check_optional_text(const char *text, const char *member, cJSON **refusal) {
    if (text != NULL && text[0] == '\0') {
        refuse(refusal, "pass each text as a non-empty string, or none", "%s is not usable text", member);
        return 0;
    }
    return 1;
}

static int check_required_text(const char *text, const char *member, cJSON **refusal) {
    if (!check_optional_text(text, member, refusal)) {
        return 0;
    }
    if (text == NULL) {
        refuse(refusal,
               "pass id, cache and plugin_directory and, when they are known, version, last_known and api_base",
               "the call carries no %s", member);
        return 0;
    }
    return 1;
}

static int read_request(const runtime_install_by_id_request *install, install_request *request, cJSON **refusal) {
    memset(request, 0, sizeof *request);
    runtime_identity parsed;
    if (!check_required_text(install->id, "id", refusal) || !check_required_text(install->cache, "cache", refusal) ||
        !check_required_text(install->plugin_directory, "plugin_directory", refusal) ||
        !check_optional_text(install->version, "version", refusal) ||
        !check_optional_text(install->last_known, "last_known", refusal) ||
        !check_optional_text(install->api_base, "api_base", refusal) ||
        !check_optional_text(install->accept_primary, "accept_primary", refusal)) {
        return 0;
    }
    request->id = install->id;
    request->cache = install->cache;
    request->plugin_directory = install->plugin_directory;
    request->version = install->version;
    request->last_known = install->last_known;
    request->api_base = install->api_base;
    request->accept_primary = install->accept_primary;
    if (!runtime_identity_parse_id(request->id, &parsed)) {
        refuse(refusal, "send an id such as github:<number>", "the id is not valid");
        return 0;
    }
    if (parsed.number.start == NULL) {
        refuse(refusal, "a local plugin is deployed from its folder, not installed", "%s names no repo host to install from",
               request->id);
        return 0;
    }
    size_t repo_length = parsed.path.length == 0 ? strlen(request->id) : (size_t)(parsed.path.start - 1 - request->id);
    char *repo_id = spisor_memory_allocate(repo_length + 1);
    if (repo_id != NULL) {
        memcpy(repo_id, request->id, repo_length);
        repo_id[repo_length] = '\0';
    }
    request->repo_id = repo_id;
    return repo_id != NULL;
}

static void refuse_fetch(const runtime_fetch_answer *answer, const char *url, cJSON **refusal) {
    if (answer->outcome == RUNTIME_FETCH_STATUS && (answer->status == 403 || answer->status == 429)) {
        refuse(refusal, "wait for the host's rate limit to reset, then try again", "%s answered HTTP %ld: an anonymous caller is rate limited by this host",
               url, answer->status);
    } else if (answer->outcome == RUNTIME_FETCH_STATUS) {
        refuse(refusal, "check that the repo has a release that publishes " INDEX_ASSET, "%s answered HTTP %ld", url,
               answer->status);
    } else if (answer->outcome == RUNTIME_FETCH_OUT_OF_MEMORY) {
        *refusal = NULL;
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

/** The bytes at url, freed and NUL terminated, with their length; NULL with a refusal, or without one out of memory. */
static uint8_t *fetch_bytes(const char *url, size_t capacity, size_t *length, cJSON **refusal) {
    uint8_t *body = spisor_memory_allocate(capacity + 1);
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
        runtime_fetch_into_memory(url, NULL, LOOKUP_DEADLINE_MILLISECONDS, &sink, &answer);
    }
    int received = answer.outcome == RUNTIME_FETCH_RECEIVED;
    if (received) {
        *length = (size_t)sink.bytes;
        body[*length] = '\0';
    } else {
        refuse_fetch(&answer, url, refusal);
    }
    spisor_memory_release(answer.etag);
    spisor_memory_release(answer.last_modified);
    if (!received) {
        spisor_memory_release(body);
        return NULL;
    }
    return body;
}

static cJSON *resolve_repository(const install_request *request, cJSON **refusal) {
    runtime_identity_lookup_request lookup = {request->repo_id, request->last_known, NULL, request->api_base};
    return runtime_identity_lookup(&lookup, refusal);
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

/**
 * The set the repo's copies agree on. The repo alone, unconfirmed, when its also-at file is unreadable or names a
 * primary nothing confirms; any refusal, copies naming different primaries included, stops the install. NULL with a
 * refusal for that, or without one out of memory.
 */
static cJSON *verified_set(const install_request *request, cJSON **refusal) {
    cJSON *found_refusal = NULL;
    cJSON *found = runtime_identity_read_also_at(request->repo_id, request->api_base, &found_refusal);
    if (found_refusal != NULL) {
        *refusal = found_refusal;
        return NULL;
    }
    if (found != NULL && has_confirmed_primary(found)) {
        return found;
    }
    cJSON_Delete(found);
    return found == NULL ? NULL : unconfirmed_set(request->repo_id);
}

/** cache/identities/<folder>.json, where the set and primary of each installed id are kept; freed. */
static char *record_path(const install_request *request) {
    int unfit = 0;
    char *folder = runtime_item_folder(request->repo_id, &unfit);
    const char *file_parts[2] = {folder, ".json"};
    char *file_name = folder == NULL ? NULL : runtime_cache_text(file_parts, 2);
    const char *segments[2] = {"identities", file_name};
    char *path = file_name == NULL ? NULL : runtime_cache_resolve(request->cache, segments, 2);
    spisor_memory_release(file_name);
    spisor_memory_release(folder);
    return path;
}

/**
 * Reads the primary recorded for the id; 1 when there is none or it reads, 0 with a refusal when the record is unusable,
 * or without one out of memory.
 */
static int read_recorded_primary(install_request *request, cJSON **refusal) {
    char *path = record_path(request);
    spisor_file_presence presence = path == NULL ? SPISOR_FILE_OUT_OF_MEMORY : spisor_probe_file(path);
    int out_of_memory = presence == SPISOR_FILE_OUT_OF_MEMORY;
    int present = presence == SPISOR_FILE_PRESENT;
    cJSON *record = present ? runtime_read_node_json(path, &out_of_memory) : NULL;
    const char *primary = text_of(record, "primary");
    if (primary != NULL) {
        request->recorded_primary = runtime_cache_text(&primary, 1);
        out_of_memory = request->recorded_primary == NULL;
    } else if (present && !out_of_memory) {
        refuse(refusal, "delete the record so the next install records it again", "%s holds no recorded primary", path);
    }
    cJSON_Delete(record);
    spisor_memory_release(path);
    return !out_of_memory && (!present || primary != NULL);
}

static int add_copy(cJSON *answer, const char *member, const cJSON *value) {
    cJSON *copy = cJSON_Duplicate(value, 1);
    if (copy == NULL || !cJSON_AddItemToObject(answer, member, copy)) {
        cJSON_Delete(copy);
        return 0;
    }
    return 1;
}

/** OK once the record is written, REFUSED when the disk refuses it, OUT_OF_MEMORY before anything is written. */
static spisor_status write_record(const install_request *request, const cJSON *set) {
    cJSON *record = cJSON_CreateObject();
    int built = record != NULL && add_copy(record, "ids", cJSON_GetObjectItemCaseSensitive(set, "ids")) &&
                cJSON_AddStringToObject(record, "primary", text_of(set, "primary")) != NULL;
    char *text = built ? cJSON_Print(record) : NULL;
    char *path = text == NULL ? NULL : record_path(request);
    const char *parent_segment[1] = {".."};
    char *directory = path == NULL ? NULL : runtime_cache_resolve(path, parent_segment, 1);
    spisor_status status = directory == NULL ? SPISOR_OUT_OF_MEMORY : spisor_make_directories(directory);
    if (status == SPISOR_OK) {
        status = spisor_replace_file(path, (const uint8_t *)text, strlen(text));
    }
    spisor_memory_release(directory);
    spisor_memory_release(path);
    cJSON_free(text);
    cJSON_Delete(record);
    return status;
}

static int needs_user_acceptance(const install_request *request, const cJSON *set, int *needed, cJSON **refusal) {
    *needed = 0;
    if (request->recorded_primary == NULL) {
        return 1;
    }
    const runtime_identity_definition *identity = runtime_identity_definition_of();
    runtime_identity_version_argument argument = SPISOR_INIT(runtime_identity_version_argument);
    runtime_identity_version_answer decision = SPISOR_INIT(runtime_identity_version_answer);
    spisor_refusal refused = SPISOR_INIT(spisor_refusal);
    argument.recorded = request->recorded_primary;
    argument.primary = text_of(set, "primary");
    spisor_status status = identity->accepts_version(identity->context, &argument, &decision, &refused);
    int accepted = request->accept_primary != NULL && strcmp(request->accept_primary, text_of(set, "primary")) == 0;
    *needed = status == SPISOR_OK && !decision.accepted && !accepted;
    *refusal = status == SPISOR_OK ? NULL : runtime_refusal_of(&refused);
    identity->release_refusal(identity->context, &refused);
    return status == SPISOR_OK;
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

/** The index url of the latest release, freed; NULL with a refusal or, out of memory, without one. */
static char *find_index_url(const install_request *request, const cJSON *repository, cJSON **refusal) {
    char url[URL_SIZE];
    const char *full_name = text_of(repository, "fullName");
    if (full_name == NULL ||
        !runtime_identity_repo_url(request->repo_id, request->api_base, full_name, "releases/latest", url, sizeof url)) {
        refuse(refusal, "use a shorter apiBase", "the address of the latest release of %s is not usable", request->id);
        return NULL;
    }
    size_t length = 0;
    uint8_t *bytes = fetch_bytes(url, RELEASE_CAPACITY, &length, refusal);
    int out_of_memory = 0;
    cJSON *release = bytes == NULL ? NULL : spisor_parse_json_reporting(bytes, length, &out_of_memory);
    int fetched = bytes != NULL && !out_of_memory;
    spisor_memory_release(bytes);
    const char *asset_url = release == NULL ? NULL : index_asset_url(release);
    if (fetched && asset_url == NULL) {
        refuse(refusal, "ask the owner to publish " INDEX_ASSET " with each release", "the latest release of %s has no %s",
               full_name, INDEX_ASSET);
    }
    char *copy = asset_url == NULL ? NULL : spisor_memory_allocate(strlen(asset_url) + 1);
    if (copy != NULL) {
        strcpy(copy, asset_url);
    }
    cJSON_Delete(release);
    return copy;
}

static cJSON *catalog_around(const char *id, const cJSON *item) {
    cJSON *catalog = cJSON_CreateObject();
    int formatted = catalog != NULL && cJSON_AddNumberToObject(catalog, "format", RUNTIME_CATALOG_FORMAT) != NULL;
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
    spisor_memory_release(message);
    runtime_catalog_free_refusal(catalog_refusal);
}

/** The index's one item, checked as a catalog item with its file urls resolved against the index's own url. */
static cJSON *read_catalog(const char *id, const char *url, const uint8_t *bytes, size_t length, cJSON **refusal) {
    int out_of_memory = 0;
    cJSON *item = spisor_parse_json_reporting(bytes, length, &out_of_memory);
    cJSON *wrapped = cJSON_IsObject(item) ? catalog_around(id, item) : NULL;
    char *text = wrapped == NULL ? NULL : cJSON_PrintUnformatted(wrapped);
    runtime_catalog_refusal catalog_refusal;
    cJSON *catalog = NULL;
    spisor_status status =
        text == NULL ? SPISOR_OK : runtime_catalog_read_text(text, strlen(text), url, &catalog, &catalog_refusal);
    if (status == SPISOR_REFUSED) {
        refuse_catalog(&catalog_refusal, url, refusal);
    } else if (status == SPISOR_OUT_OF_MEMORY) {
        runtime_catalog_free_refusal(&catalog_refusal);
    } else if (text == NULL && item != NULL && !cJSON_IsObject(item)) {
        refuse(refusal, "publish the catalog item as a JSON object", "%s is not a JSON object", url);
    } else if (text == NULL && item == NULL && !out_of_memory) {
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
               "%s was %s when the install began and is %s now", request->repo_id, text_of(repository, "fullName"),
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
    spisor_memory_release(bytes);
    spisor_memory_release(url);
    return index->item != NULL && is_plugin_item(index->item, refusal);
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
    runtime_cache_deploy_request deploy = {text_of(installed, "directory"), request->plugin_directory, request->id,
                                           text_of(repository, "name"), request->replaces};
    int succeeded = deploy.directory != NULL && runtime_cache_deploy(&deploy, refusal);
    cJSON_Delete(installed);
    return succeeded ? version : NULL;
}

static int add_text(cJSON *answer, const char *member, const char *value) {
    return cJSON_AddStringToObject(answer, member, value == NULL ? "" : value) != NULL;
}

static cJSON *set_answer(const install_request *request, const cJSON *repository, const cJSON *set) {
    cJSON *answer = cJSON_CreateObject();
    int built = answer != NULL && add_text(answer, "id", request->id) && add_text(answer, "name", text_of(repository, "name")) &&
                add_text(answer, "fullName", text_of(repository, "fullName")) &&
                add_copy(answer, "ids", cJSON_GetObjectItemCaseSensitive(set, "ids")) &&
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

/** Hands the answer to the request's taker before the record is written, so no answer is lost after it. */
static spisor_status take_answer(const install_request *request, const cJSON *answer) {
    return request->take == NULL ? SPISOR_OK : request->take(answer, request->taken);
}

/** Installs from source, the repo the verified set names as primary, and records the set under the id that was asked for. */
static cJSON *install_verified(const install_request *request, const install_request *source, const cJSON *repository,
                               const cJSON *set, cJSON **refusal) {
    int needed = 0;
    if (!needs_user_acceptance(request, set, &needed, refusal)) {
        return NULL;
    }
    if (needed) {
        return acceptance_answer(source, repository, set);
    }
    release_index index;
    cJSON *answer = NULL;
    if (read_release_index(source, repository, &index, refusal)) {
        const char *version = install_and_deploy(source, repository, &index, refusal);
        answer = version == NULL ? NULL : installed_answer(source, repository, set, &index, version);
        spisor_status recorded = answer == NULL ? SPISOR_OK : take_answer(request, answer);
        if (recorded == SPISOR_OK && answer != NULL) {
            recorded = write_record(request, set);
        }
        if (recorded == SPISOR_REFUSED) {
            refuse(refusal, "free the cache's identities folder or point cache at a writable directory, then install again",
                   "the primary of %s could not be recorded", request->repo_id);
        }
        if (recorded != SPISOR_OK) {
            cJSON_Delete(answer);
            answer = NULL;
        }
    }
    cJSON_Delete(index.catalog);
    return answer;
}

/** The id of a folder at repo_id, the repo id followed by the folder as asked for. */
static char *joined_id(const char *repo_id, const char *folder) {
    const char *parts[2] = {repo_id, folder};
    return runtime_cache_text(parts, 2);
}

/** The id of the folder below a repo id as asked for, "/kernel" for github:5/kernel and "" for a repo. */
static const char *folder_of(const install_request *request) {
    return request->id + strlen(request->repo_id);
}

/**
 * A confirmed primary other than the repo asked for is where a new version comes from, at the same folder; a mirror
 * never supplies one. An accepted change of primary lets the deployed plugin of the old primary be replaced.
 */
static install_request source_of(const install_request *request, const cJSON *set, char *source_id, char *replaced_id) {
    install_request source = *request;
    const char *primary = text_of(set, "primary");
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(set, "primaryConfirmed")) && primary != NULL &&
        strcmp(primary, request->repo_id) != 0) {
        source.repo_id = primary;
        source.last_known = NULL;
    }
    source.id = source_id;
    int accepted_change = request->recorded_primary != NULL && request->accept_primary != NULL && primary != NULL &&
                          strcmp(request->recorded_primary, primary) != 0 && strcmp(request->accept_primary, primary) == 0;
    source.replaces = accepted_change ? replaced_id : NULL;
    return source;
}

static cJSON *install_from_set(const install_request *request, const cJSON *repository, const cJSON *set,
                               cJSON **refusal) {
    char *source_id = joined_id(text_of(set, "primary") != NULL && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(set, "primaryConfirmed"))
                                    ? text_of(set, "primary")
                                    : request->repo_id,
                                folder_of(request));
    char *replaced_id = request->recorded_primary == NULL ? NULL : joined_id(request->recorded_primary, folder_of(request));
    cJSON *answer = NULL;
    if (source_id != NULL && (request->recorded_primary == NULL || replaced_id != NULL)) {
        install_request source = source_of(request, set, source_id, replaced_id);
        int is_mirror = strcmp(source.repo_id, request->repo_id) != 0;
        cJSON *primary_repository = is_mirror ? resolve_repository(&source, refusal) : NULL;
        answer = is_mirror && primary_repository == NULL
                     ? NULL
                     : install_verified(request, &source, is_mirror ? primary_repository : repository, set, refusal);
        cJSON_Delete(primary_repository);
    }
    spisor_memory_release(replaced_id);
    spisor_memory_release(source_id);
    return answer;
}


cJSON *runtime_install_by_id(const runtime_install_by_id_request *install, runtime_install_by_id_taker take,
                             void *taken, cJSON **refusal) {
    install_request request;
    int read;
    *refusal = NULL;
    read = read_request(install, &request, refusal);
    request.take = take;
    request.taken = taken;
    if (!read || !read_recorded_primary(&request, refusal)) {
        spisor_memory_release(request.recorded_primary);
        spisor_memory_release((char *)request.repo_id);
        return NULL;
    }
    cJSON *repository = resolve_repository(&request, refusal);
    cJSON *set = repository == NULL ? NULL : verified_set(&request, refusal);
    cJSON *answer = set == NULL ? NULL : install_from_set(&request, repository, set, refusal);
    cJSON_Delete(set);
    cJSON_Delete(repository);
    spisor_memory_release(request.recorded_primary);
    spisor_memory_release((char *)request.repo_id);
    return answer;
}
