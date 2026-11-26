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
#include "identity.h"
#include "identity_naula.h"
#include "json_service.h"
#include "naula/memory.h"
#include "naula/sha256.h"
#include "platform_key.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"

#define INDEX_ASSET "weivel-index.json"
#define MESSAGE_SIZE 2048
#define CACHE_PROBLEM_SIZE 512

/** One install, and what kept the cache from holding naula's confirmations, empty when nothing did. */
typedef struct install_request {
    const runtime_install_by_id_request *install;
    runtime_install_by_id_taker take;
    void *taken;
    char cache_problem[CACHE_PROBLEM_SIZE];
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
        refuse(refusal, "pass id, at, cache and plugin_directory and, when they are known, version",
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
           check_optional_text(install->accept_number, "accept_number", refusal);
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

static int is_plugin_item(const cJSON *item, cJSON **refusal) {
    const char *kind = text_of(item, "kind");
    if (kind != NULL && strcmp(kind, "plugin") == 0) {
        return 1;
    }
    refuse(refusal, "install.byId installs plugins", "the release index describes an item of kind %s", kind == NULL ? "(none)" : kind);
    return 0;
}

/**
 * Reads the index of the primary's latest release through the primary's provider, by number, and checks its one item;
 * 0 with a refusal, or without one out of memory.
 */
static int read_release_index(const install_request *request, const primary_home *primary, release_index *index,
                              cJSON **refusal) {
    memset(index, 0, sizeof *index);
    naula_providers *providers = NULL;
    naula_refusal refused = empty_refusal();
    naula_release_index read;
    memset(&read, 0, sizeof read);
    read.size = sizeof read;
    naula_status status = runtime_identity_providers(&providers);
    const naula_provider *provider =
        status == NAULA_OK ? naula_providers_find(providers, primary->provider, &refused) : NULL;
    if (status == NAULA_OK && provider == NULL) {
        status = NAULA_REFUSED;
    }
    if (provider != NULL) {
        status = provider->release_index(provider->context, primary->host, primary->number, NULL, INDEX_ASSET, NULL,
                                         &read, &refused);
    }
    if (status != NAULA_OK && status != NAULA_OUT_OF_MEMORY) {
        refuse_naula(&refused, refusal);
    }
    const char *id = request->install->id;
    if (status == NAULA_OK) {
        index->catalog = read_catalog(id, read.url, read.bytes, read.length, refusal);
        digest_of(read.bytes, read.length, index->sha256);
    }
    naula_release_index_release(&read);
    index->item = index->catalog == NULL
                      ? NULL
                      : cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(index->catalog, "items"), id);
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

static int add_dispute(cJSON *disputes, const runtime_identity_disputed *disputed) {
    cJSON *row = cJSON_CreateObject();
    if (row == NULL || !add_text(row, "account", disputed->account) ||
        !add_text(row, "recordedNumber", disputed->recorded_number) ||
        !add_text(row, "offeredNumber", disputed->offered_number) || !add_text(row, "accept", disputed->accept) ||
        !cJSON_AddItemToArray(disputes, row)) {
        cJSON_Delete(row);
        return 0;
    }
    return 1;
}

static int add_disputes(cJSON *answer, const runtime_identity_acceptance *acceptance) {
    cJSON *disputes = cJSON_AddArrayToObject(answer, "disputes");
    for (size_t index = 0; disputes != NULL && index < acceptance->dispute_count; index++) {
        if (!add_dispute(disputes, &acceptance->disputes[index])) {
            return 0;
        }
    }
    return disputes != NULL;
}

static cJSON *acceptance_answer(const install_request *request, const runtime_identity_acceptance *acceptance) {
    cJSON *answer = cJSON_CreateObject();
    int built = answer != NULL && add_text(answer, "id", request->install->id);
    if (built && acceptance->primary_needed) {
        built = add_text(answer, "primary", acceptance->offered_primary) &&
                add_text(answer, "recordedPrimary", acceptance->recorded_primary) &&
                cJSON_AddTrueToObject(answer, "needsUserAcceptance") != NULL;
    } else if (built) {
        built = add_disputes(answer, acceptance) && cJSON_AddTrueToObject(answer, "needsNumberAcceptance") != NULL;
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
                add_text(answer, "commit", commit_of(index->item, version)) && add_text(answer, "sha256", index->sha256) &&
                (request->cache_problem[0] == '\0' || add_text(answer, "cacheProblem", request->cache_problem));
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

static cJSON *install_resolved(install_request *request, char *repo_id, cJSON **refusal) {
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
    if (request->cache_problem[0] == '\0' && resolution.cache_problem[0] != '\0') {
        snprintf(request->cache_problem, sizeof request->cache_problem, "%s", resolution.cache_problem);
    }
    if (runtime_identity_needs_the_user(status, &refused, &acceptance)) {
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
    install_request request;
    memset(&request, 0, sizeof request);
    request.install = install;
    request.take = take;
    request.taken = taken;
    *refusal = NULL;
    if (!check_texts(install, refusal) || !check_id(install->id, refusal)) {
        return NULL;
    }
    spisor_status made = spisor_make_directories(install->cache);
    if (made == SPISOR_OUT_OF_MEMORY) {
        return NULL;
    }
    if (made != SPISOR_OK) {
        snprintf(request.cache_problem, sizeof request.cache_problem,
                 "the cache place %s could not be made, so naula keeps no confirmation of this install", install->cache);
    }
    char *repo_id = runtime_identity_repo_of(install->id);
    cJSON *answer = repo_id == NULL ? NULL : install_resolved(&request, repo_id, refusal);
    spisor_memory_release(repo_id);
    return answer;
}
