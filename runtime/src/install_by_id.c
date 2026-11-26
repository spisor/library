#include "install_by_id.h"
#include "allocation.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache_deploy.h"
#include "cache_install.h"
#include "catalog.h"
#include "catalog_read.h"
#include "fetch_answer.h"
#include "fetch_file.h"
#include "identity.h"
#include "identity_naula.h"
#include "json_service.h"
#include "naula/memory.h"
#include "naula/sha256.h"
#include "platform_key.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"

#define INDEX_ASSET "weivel-index.json"
#define RELEASE_CAPACITY (256 * 1024)
#define INDEX_CAPACITY (4 * 1024 * 1024)
#define LOOKUP_DEADLINE_MILLISECONDS 30000u
#define URL_SIZE 1024
#define MESSAGE_SIZE 2048
#define ROOT_SIZE 512

typedef struct install_request {
    const runtime_install_by_id_request *install;
    runtime_install_by_id_taker take;
    void *taken;
} install_request;

/** The primary naula confirmed, as the release is read from it: its provider, host, number, account and name. */
typedef struct primary_home {
    const char *provider;
    const char *host;
    uint64_t number;
    const char *account;
    const char *name;
    char place[RUNTIME_PLACE_TEXT_SIZE];
} primary_home;

typedef struct release_index {
    cJSON *catalog;
    const cJSON *item;
    char sha256[NAULA_SHA256_HEX_SIZE];
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
        refuse(refusal, "pass id, at, cache and plugin_directory and, when they are known, version and api_base",
               "the call carries no %s", member);
        return 0;
    }
    return 1;
}

static int check_texts(const runtime_install_by_id_request *install, cJSON **refusal) {
    return check_required_text(install->id, "id", refusal) && check_required_text(install->cache, "cache", refusal) &&
           check_required_text(install->plugin_directory, "plugin_directory", refusal) &&
           check_optional_text(install->version, "version", refusal) &&
           check_optional_text(install->accept_primary, "accept_primary", refusal) &&
           check_optional_text(install->accept_number, "accept_number", refusal) &&
           check_optional_text(install->api_base, "api_base", refusal);
}

/** 1 for an id of a home's repo; 0 with a refusal, or without one out of memory. */
static int check_id(const char *id, cJSON **refusal) {
    runtime_identity_kind kind = runtime_identity_kind_of(id);
    if (kind == RUNTIME_IDENTITY_NOT_AN_ID) {
        refuse(refusal, "send an id such as git:<root commit>", "the id is not valid");
    } else if (kind == RUNTIME_IDENTITY_LOCAL) {
        refuse(refusal, "a local plugin is deployed from its folder, not installed", "%s names no home to install from",
               id);
    }
    return kind == RUNTIME_IDENTITY_GIT;
}

static int check_aim(const runtime_install_by_id_request *install, cJSON **refusal) {
    if (install->api_base != NULL && !naula_fetch_plain_http_is_allowed()) {
        refuse(refusal, "pass no api_base: the release host's own API address is used",
               "api_base is only honoured by the test build");
        return 0;
    }
    return 1;
}

static void refuse_fetch(const naula_fetch_answer *answer, const char *url, cJSON **refusal) {
    if (answer->outcome == NAULA_FETCH_STATUS && (answer->status == 403 || answer->status == 429)) {
        refuse(refusal, "wait for the host's rate limit to reset, then try again", "%s answered HTTP %ld: an anonymous caller is rate limited by this host",
               url, answer->status);
    } else if (answer->outcome == NAULA_FETCH_STATUS) {
        refuse(refusal, "check that the repo has a release that publishes " INDEX_ASSET, "%s answered HTTP %ld", url,
               answer->status);
    } else if (answer->outcome == NAULA_FETCH_OUT_OF_MEMORY) {
        *refusal = NULL;
    } else if (answer->outcome == NAULA_FETCH_REFUSED || runtime_fetch_is_deadline_cut_off(answer)) {
        refuse(refusal, answer->fix, "%s", answer->detail);
    } else if (answer->outcome == NAULA_FETCH_STOPPED_BY_SINK) {
        refuse(refusal, "check that the release publishes the files it should", "%s answered more than an install reads", url);
    } else {
        refuse(refusal, NAULA_FETCH_NETWORK_FIX, "%s ended before its whole answer arrived", url);
    }
}

/** A file: url is read only when the fetch test hook allows plain http, so only a test can aim an install at a local release. */
static int is_readable_file_url(const char *url) {
    return strncmp(url, "file:", 5) == 0 && naula_fetch_plain_http_is_allowed();
}

/** The bytes at url, freed and NUL terminated, with their length; NULL with a refusal, or without one out of memory. */
static uint8_t *fetch_bytes(const char *url, size_t capacity, size_t *length, cJSON **refusal) {
    uint8_t *body = spisor_memory_allocate(capacity + 1);
    if (body == NULL) {
        return NULL;
    }
    naula_fetch_sink sink;
    naula_fetch_sink_start_memory(&sink, body, capacity);
    naula_fetch_answer answer;
    memset(&answer, 0, sizeof answer);
    answer.size = sizeof answer;
    if (is_readable_file_url(url)) {
        runtime_fetch_file_url(url, &sink, &answer);
    } else {
        naula_fetch_into_memory(url, NULL, LOOKUP_DEADLINE_MILLISECONDS, &sink, &answer);
    }
    int received = answer.outcome == NAULA_FETCH_RECEIVED;
    if (received) {
        *length = (size_t)sink.bytes;
        body[*length] = '\0';
    } else {
        refuse_fetch(&answer, url, refusal);
    }
    naula_memory_release(answer.etag);
    naula_memory_release(answer.last_modified);
    if (!received) {
        spisor_memory_release(body);
        return NULL;
    }
    return body;
}

static void refuse_naula(const naula_refusal *refused, cJSON **refusal) {
    char message[MESSAGE_SIZE];
    runtime_identity_refusal_text(refused, message, sizeof message);
    *refusal = runtime_refusal(message, runtime_identity_refusal_fix(refused));
}

static naula_refusal empty_refusal(void) {
    naula_refusal refused;
    memset(&refused, 0, sizeof refused);
    refused.size = sizeof refused;
    return refused;
}

/** The address of the latest release under the primary's API root; 0 with a refusal when it has none here. */
static int latest_release_url(const install_request *request, const primary_home *primary, char *url,
                              cJSON **refusal) {
    char root[ROOT_SIZE];
    const char *api_base = request->install->api_base;
    int fits;
    if (api_base != NULL) {
        fits = snprintf(root, sizeof root, "%s", api_base) < (int)sizeof root;
    } else if (strcmp(primary->provider, "github") == 0) {
        fits = primary->host == NULL || strcmp(primary->host, "github.com") == 0
                   ? snprintf(root, sizeof root, "https://api.github.com") < (int)sizeof root
                   : snprintf(root, sizeof root, "https://%s/api/v3", primary->host) < (int)sizeof root;
    } else if ((strcmp(primary->provider, "gitea") == 0 || strcmp(primary->provider, "forgejo") == 0) &&
               primary->host != NULL) {
        fits = snprintf(root, sizeof root, "https://%s/api/v1", primary->host) < (int)sizeof root;
    } else {
        refuse(refusal, "install from a repo whose primary is on GitHub, Gitea or Forgejo",
               "%s is the primary of %s, and releases are read here only from GitHub, Gitea and Forgejo", primary->place,
               request->install->id);
        return 0;
    }
    size_t length = strlen(root);
    while (length > 0 && root[length - 1] == '/') {
        root[--length] = '\0';
    }
    if (!fits || snprintf(url, URL_SIZE, "%s/repos/%s/%s/releases/latest", root, primary->account, primary->name) >=
                     URL_SIZE) {
        refuse(refusal, "use a shorter apiBase", "the address of the latest release of %s is not usable",
               request->install->id);
        return 0;
    }
    return 1;
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

/** The index url of the latest release, freed; NULL with a refusal or, out of memory, without one. */
static char *find_index_url(const install_request *request, const primary_home *primary, cJSON **refusal) {
    char url[URL_SIZE];
    if (!latest_release_url(request, primary, url, refusal)) {
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
        refuse(refusal, "ask the owner to publish " INDEX_ASSET " with each release", "the latest release of %s/%s has no %s",
               primary->account, primary->name, INDEX_ASSET);
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

static void digest_of(const uint8_t *bytes, size_t length, char sha256[NAULA_SHA256_HEX_SIZE]) {
    naula_sha256 hash;
    naula_sha256_start(&hash);
    naula_sha256_add(&hash, bytes, length);
    naula_sha256_hex(&hash, sha256);
}

/** 1 when the primary's number still answers its account and name; 0 with a refusal, or without one out of memory. */
static int is_still_the_primary(const primary_home *primary, cJSON **refusal) {
    naula_providers *providers = NULL;
    naula_refusal refused = empty_refusal();
    naula_status status = runtime_identity_providers(&providers);
    const naula_provider *provider = status == NAULA_OK ? naula_providers_find(providers, primary->provider, &refused) : NULL;
    naula_repository again;
    memset(&again, 0, sizeof again);
    again.size = sizeof again;
    if (provider != NULL) {
        status = provider->lookup(provider->context, primary->host, primary->number, NULL, &again, &refused);
    }
    int same = status == NAULA_OK && provider != NULL && again.account != NULL && again.name != NULL &&
               strcmp(again.account, primary->account) == 0 && strcmp(again.name, primary->name) == 0;
    if (status == NAULA_OK && provider != NULL && !same) {
        refuse(refusal, "try the install again; if it keeps happening, check who owns the repo now",
               "%s was %s/%s when the install began and is %s/%s now", primary->place, primary->account, primary->name,
               again.account == NULL ? "" : again.account, again.name == NULL ? "" : again.name);
    } else if (status != NAULA_OK && status != NAULA_OUT_OF_MEMORY) {
        refuse_naula(&refused, refusal);
    } else if (status == NAULA_OK && provider == NULL) {
        refuse_naula(&refused, refusal);
    }
    naula_repository_release(&again);
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

static int read_release_index(const install_request *request, const primary_home *primary, release_index *index,
                              cJSON **refusal) {
    memset(index, 0, sizeof *index);
    char *url = find_index_url(request, primary, refusal);
    size_t length = 0;
    uint8_t *bytes = url == NULL ? NULL : fetch_bytes(url, INDEX_CAPACITY, &length, refusal);
    int tied = bytes != NULL && is_still_the_primary(primary, refusal);
    const char *id = request->install->id;
    index->catalog = tied ? read_catalog(id, url, bytes, length, refusal) : NULL;
    index->item = index->catalog == NULL
                      ? NULL
                      : cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(index->catalog, "items"), id);
    if (bytes != NULL) {
        digest_of(bytes, length, index->sha256);
    }
    spisor_memory_release(bytes);
    spisor_memory_release(url);
    return index->item != NULL && is_plugin_item(index->item, refusal);
}

static const char *chosen_version(const install_request *request, const release_index *index, cJSON **refusal) {
    const char *version = request->install->version != NULL ? request->install->version : text_of(index->item, "latest");
    if (version == NULL) {
        refuse(refusal, "ask the owner to name a latest version in the release index", "the release index names no latest version");
    }
    return version;
}

static int add_text(cJSON *answer, const char *member, const char *value) {
    return cJSON_AddStringToObject(answer, member, value == NULL ? "" : value) != NULL;
}

static int add_homes(cJSON *answer, const naula_resolution *resolution) {
    cJSON *homes = cJSON_AddArrayToObject(answer, "homes");
    for (size_t index = 0; homes != NULL && index < resolution->home_count; index++) {
        char text[RUNTIME_PLACE_TEXT_SIZE];
        cJSON *home = runtime_identity_place_text(&resolution->homes[index], text, sizeof text)
                          ? cJSON_CreateString(text)
                          : NULL;
        if (home == NULL || !cJSON_AddItemToArray(homes, home)) {
            cJSON_Delete(home);
            return 0;
        }
    }
    return homes != NULL;
}

static cJSON *acceptance_answer(const install_request *request, const runtime_identity_acceptance *acceptance) {
    cJSON *answer = cJSON_CreateObject();
    int built = answer != NULL && add_text(answer, "id", request->install->id);
    if (built && acceptance->primary_needed) {
        built = add_text(answer, "primary", acceptance->offered_primary) &&
                add_text(answer, "recordedPrimary", acceptance->recorded_primary) &&
                cJSON_AddTrueToObject(answer, "needsUserAcceptance") != NULL;
    } else if (built) {
        built = add_text(answer, "disputedAccount", acceptance->disputed_account) &&
                add_text(answer, "recordedNumber", acceptance->recorded_number) &&
                add_text(answer, "offeredNumber", acceptance->offered_number) &&
                cJSON_AddTrueToObject(answer, "needsNumberAcceptance") != NULL;
    }
    if (!built) {
        cJSON_Delete(answer);
        answer = NULL;
    }
    return answer;
}

static const char *commit_of(const cJSON *item, const char *version) {
    return text_of(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(item, "versions"), version), "commit");
}

static cJSON *installed_answer(const install_request *request, const primary_home *primary,
                               const naula_resolution *resolution, const release_index *index, const char *version) {
    char full_name[RUNTIME_PLACE_TEXT_SIZE];
    snprintf(full_name, sizeof full_name, "%s/%s", primary->account, primary->name);
    cJSON *answer = cJSON_CreateObject();
    int built = answer != NULL && add_text(answer, "id", request->install->id) && add_text(answer, "name", primary->name) &&
                add_text(answer, "fullName", full_name) && add_homes(answer, resolution) &&
                add_text(answer, "primary", primary->place) && add_text(answer, "version", version) &&
                add_text(answer, "commit", commit_of(index->item, version)) && add_text(answer, "sha256", index->sha256);
    if (!built) {
        cJSON_Delete(answer);
        answer = NULL;
    }
    return answer;
}

/** The primary of a confirmed resolution, as a release is read from it; 0 with a refusal when it has none. */
static int primary_of(const install_request *request, const naula_resolution *resolution, primary_home *primary,
                      cJSON **refusal) {
    memset(primary, 0, sizeof *primary);
    const naula_place *place = resolution->home_count == 0 ? NULL : &resolution->homes[resolution->primary];
    if (place == NULL || !runtime_identity_place_text(place, primary->place, sizeof primary->place)) {
        refuse(refusal, RUNTIME_IDENTITY_FIX, "nothing confirms %s", request->install->id);
        return 0;
    }
    if (resolution->state != NAULA_CONFIRMED || place->kind != NAULA_HOME_PROVIDER ||
        resolution->primary_repository.account == NULL || resolution->primary_repository.name == NULL) {
        refuse(refusal, "try again once its primary can be reached",
               "%s could not be confirmed at its primary %s now, so no release can be read from it",
               request->install->id, primary->place);
        return 0;
    }
    primary->provider = place->provider;
    primary->host = place->host;
    primary->number = place->number;
    primary->account = resolution->primary_repository.account;
    primary->name = resolution->primary_repository.name;
    return 1;
}

/** Hands the answer to the request's taker, so the caller holds it before it is answered. */
static spisor_status take_answer(const install_request *request, const cJSON *answer) {
    return request->take == NULL ? SPISOR_OK : request->take(answer, request->taken);
}

/** Deploys the installed version, the install's last write; 0 with a refusal, or without one out of memory. */
static int deploy(const install_request *request, const primary_home *primary, const cJSON *installed,
                  cJSON **refusal) {
    const runtime_install_by_id_request *install = request->install;
    runtime_cache_deploy_request deployment = {text_of(installed, "directory"), install->plugin_directory, install->id,
                                               primary->name, NULL};
    return deployment.directory != NULL && runtime_cache_deploy(&deployment, refusal);
}

/**
 * Installs the chosen version into the cache, takes the answer and only then deploys, so no allocation can fail after
 * the deploy has written the plugin; NULL with a refusal, or without one out of memory.
 */
static cJSON *install_release(const install_request *request, const primary_home *primary,
                              const naula_resolution *resolution, const release_index *index, cJSON **refusal) {
    const runtime_install_by_id_request *install = request->install;
    const char *version = chosen_version(request, index, refusal);
    cJSON *installed = version == NULL ? NULL
                                       : runtime_cache_install_version(install->cache, install->id, index->item, version,
                                                                       runtime_platform_key(), refusal);
    cJSON *answer = installed == NULL ? NULL : installed_answer(request, primary, resolution, index, version);
    if (answer != NULL && (take_answer(request, answer) != SPISOR_OK || !deploy(request, primary, installed, refusal))) {
        cJSON_Delete(answer);
        answer = NULL;
    }
    cJSON_Delete(installed);
    return answer;
}

static cJSON *install_from_primary(const install_request *request, const naula_resolution *resolution,
                                   cJSON **refusal) {
    primary_home primary;
    if (!primary_of(request, resolution, &primary, refusal)) {
        return NULL;
    }
    release_index index;
    cJSON *answer = read_release_index(request, &primary, &index, refusal)
                        ? install_release(request, &primary, resolution, &index, refusal)
                        : NULL;
    cJSON_Delete(index.catalog);
    return answer;
}

static cJSON *install_resolved(const install_request *request, char *repo_id, cJSON **refusal) {
    const runtime_install_by_id_request *install = request->install;
    runtime_identity_request resolving = {repo_id, install->at, install->at_count, install->cache, NULL, NULL,
                                          install->accept_primary, install->accept_number};
    naula_resolution resolution;
    memset(&resolution, 0, sizeof resolution);
    resolution.size = sizeof resolution;
    runtime_identity_acceptance acceptance;
    naula_refusal refused = empty_refusal();
    naula_status status = runtime_identity_resolve(&resolving, &resolution, &acceptance, &refused);
    cJSON *answer = NULL;
    if (status != NAULA_OUT_OF_MEMORY && (acceptance.primary_needed || acceptance.number_needed)) {
        answer = acceptance_answer(request, &acceptance);
    } else if (status == NAULA_OK) {
        answer = install_from_primary(request, &resolution, refusal);
    } else if (status != NAULA_OUT_OF_MEMORY) {
        refuse_naula(&refused, refusal);
    }
    naula_resolution_release(&resolution);
    return answer;
}

cJSON *runtime_install_by_id(const runtime_install_by_id_request *install, runtime_install_by_id_taker take,
                             void *taken, cJSON **refusal) {
    install_request request = {install, take, taken};
    *refusal = NULL;
    if (!check_texts(install, refusal) || !check_id(install->id, refusal) || !check_aim(install, refusal)) {
        return NULL;
    }
    if (spisor_make_directories(install->cache) == SPISOR_OUT_OF_MEMORY) {
        return NULL;
    }
    char *repo_id = runtime_identity_repo_of(install->id);
    cJSON *answer = repo_id == NULL ? NULL : install_resolved(&request, repo_id, refusal);
    spisor_memory_release(repo_id);
    return answer;
}
