#include "host_definition.h"

#include "json_value.h"

static const char *const START_RUNTIMES[] = {NULL, "javascript", "jvm", "native"};

static int add_text(cJSON *object, const char *name, const char *text) {
    return text == NULL || cJSON_AddStringToObject(object, name, text) != NULL;
}

static int add_owned(cJSON *object, const char *name, cJSON *item) {
    if (item == NULL || !cJSON_AddItemToObject(object, name, item)) {
        cJSON_Delete(item);
        return 0;
    }
    return 1;
}

static int add_to_array(cJSON *array, cJSON *item) {
    if (item == NULL || !cJSON_AddItemToArray(array, item)) {
        cJSON_Delete(item);
        return 0;
    }
    return 1;
}

static int add_value(cJSON *object, const char *name, const spisor_value *value) {
    cJSON *json = NULL;
    return value == NULL || (spisor_json_of_value(value, &json) == SPISOR_OK && add_owned(object, name, json));
}

cJSON *host_refusal_json(const spisor_refusal *refusal) {
    cJSON *json = refusal->detail == NULL ? NULL : cJSON_CreateObject();
    if (json != NULL && (!add_text(json, "message", refusal->detail) ||
                         !add_text(json, "fix", refusal->fix == NULL ? "" : refusal->fix))) {
        cJSON_Delete(json);
        json = NULL;
    }
    return json;
}

static int add_provenance(cJSON *registration, const runtime_registry_provenance *provenance) {
    cJSON *json = provenance == NULL ? NULL : cJSON_AddObjectToObject(registration, "provenance");
    return provenance == NULL ||
           (json != NULL && add_text(json, "method", provenance->method) &&
            add_text(json, "package", provenance->package) && add_text(json, "version", provenance->version) &&
            add_text(json, "location", provenance->location) && add_text(json, "os", provenance->os) && add_text(json, "architecture", provenance->architecture) &&
            add_text(json, "installedAt", provenance->installed_at) && add_value(json, "options", provenance->options));
}

static int add_start(cJSON *registration, const runtime_registry_start *start) {
    int known = start != NULL && start->runtime >= RUNTIME_REGISTRY_START_JAVASCRIPT &&
                start->runtime <= RUNTIME_REGISTRY_START_NATIVE;
    cJSON *json = start == NULL ? NULL : cJSON_AddObjectToObject(registration, "start");
    return start == NULL || (json != NULL && add_text(json, "runtime", known ? START_RUNTIMES[start->runtime] : NULL) &&
                             add_text(json, "entry", start->entry));
}

cJSON *host_registration_json(const runtime_registration *registration) {
    cJSON *json = cJSON_CreateObject();
    if (json == NULL || !add_text(json, "id", registration->id) || !add_text(json, "kind", "host") ||
        !add_text(json, "version", registration->version) || !add_text(json, "home", registration->home) ||
        cJSON_AddBoolToObject(json, "requiresCommand", registration->requires_command != 0) == NULL ||
        !add_value(json, "application", registration->application) ||
        !add_provenance(json, registration->provenance) || !add_start(json, registration->start)) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static cJSON *texts_json(const char *const *texts, size_t count) {
    cJSON *json = cJSON_CreateArray();
    int complete = json != NULL;
    for (size_t index = 0; complete && index < count; index++) {
        complete = add_to_array(json, cJSON_CreateString(texts[index]));
    }
    if (!complete) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static cJSON *listed_host(const runtime_registry_host *host) {
    cJSON *entry = cJSON_CreateObject();
    if (entry == NULL || !add_owned(entry, "registration", host_registration_json(host->registration)) ||
        cJSON_AddBoolToObject(entry, "orphan", host->orphan != 0) == NULL) {
        cJSON_Delete(entry);
        return NULL;
    }
    return entry;
}

static cJSON *host_listing_json(const runtime_registry_listing *listing) {
    cJSON *json = cJSON_CreateObject();
    cJSON *hosts = json == NULL ? NULL : cJSON_AddArrayToObject(json, "hosts");
    int complete = hosts != NULL;
    for (size_t index = 0; complete && index < listing->host_count; index++) {
        complete = add_to_array(hosts, listed_host(&listing->hosts[index]));
    }
    complete = complete && add_owned(json, "unreadable", texts_json(listing->unreadable, listing->unreadable_count));
    if (!complete) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static cJSON *versions_json(const runtime_cache_versions *versions, size_t count) {
    cJSON *json = cJSON_CreateObject();
    int complete = json != NULL;
    for (size_t index = 0; complete && index < count; index++) {
        const runtime_cache_versions *listed = &versions[index];
        complete = add_owned(json, listed->id, texts_json(listed->versions, listed->version_count));
    }
    if (!complete) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static cJSON *host_installed_json(const runtime_cache_installed_answer *installed) {
    cJSON *json = cJSON_CreateObject();
    if (json == NULL || !add_owned(json, "bundles", versions_json(installed->bundles, installed->bundle_count)) ||
        !add_owned(json, "libraries", versions_json(installed->libraries, installed->library_count))) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static cJSON *damage_json(const runtime_cache_damage *damage) {
    cJSON *json = cJSON_CreateObject();
    if (json == NULL || !add_text(json, "detail", damage->detail) || !add_text(json, "fix", damage->fix) ||
        !add_text(json, "file", damage->file) || !add_text(json, "version", damage->version) ||
        !add_text(json, "id", damage->id)) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static cJSON *host_damaged_json(const runtime_cache_damaged_answer *damaged) {
    cJSON *json = cJSON_CreateArray();
    int complete = json != NULL;
    for (size_t index = 0; complete && index < damaged->damaged_count; index++) {
        complete = add_to_array(json, damage_json(&damaged->damaged[index]));
    }
    if (!complete) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static cJSON *drift_json(const runtime_change_drift *drift) {
    cJSON *json = cJSON_CreateObject();
    if (json == NULL || cJSON_AddNumberToObject(json, "sequence", (double)drift->sequence) == NULL ||
        !add_text(json, "kind", drift->kind) || !add_text(json, "detail", drift->detail)) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static cJSON *host_removal_json(const runtime_hosts_remove_answer *removal) {
    cJSON *json = cJSON_CreateObject();
    int counted = json != NULL && cJSON_AddNumberToObject(json, "reversed", (double)removal->reversed) != NULL;
    cJSON *drifted = counted ? cJSON_AddArrayToObject(json, "drifted") : NULL;
    int complete = drifted != NULL;
    for (size_t index = 0; complete && index < removal->drifted_count; index++) {
        complete = add_to_array(drifted, drift_json(&removal->drifted[index]));
    }
    if (!complete || cJSON_AddBoolToObject(json, "forgotten", removal->forgotten != 0) == NULL) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static cJSON *host_sweep_json(const runtime_cache_sweep_answer *sweep) {
    cJSON *json = cJSON_CreateObject();
    if (json == NULL || !add_owned(json, "removed", texts_json(sweep->removed, sweep->removed_count))) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

cJSON *host_listing_of(const char *state, cJSON **refusal) {
    const runtime_registry_definition *registry = runtime_registry_definition_of();
    runtime_registry_list_argument argument = SPISOR_INIT(runtime_registry_list_argument);
    runtime_registry_listing listing = SPISOR_INIT(runtime_registry_listing);
    spisor_refusal declined = SPISOR_INIT(spisor_refusal);
    argument.state = state;
    spisor_status status = registry->list(registry->context, &argument, &listing, &declined);
    cJSON *json = status == SPISOR_OK ? host_listing_json(&listing) : NULL;
    *refusal = status == SPISOR_OK ? NULL : host_refusal_json(&declined);
    registry->release_listing(registry->context, &listing);
    registry->release_refusal(registry->context, &declined);
    return json;
}

cJSON *host_installed_of(const char *cache_place, cJSON **refusal) {
    const runtime_cache_definition *cache = runtime_cache_definition_of();
    runtime_cache_place_argument argument = SPISOR_INIT(runtime_cache_place_argument);
    runtime_cache_installed_answer installed = SPISOR_INIT(runtime_cache_installed_answer);
    spisor_refusal declined = SPISOR_INIT(spisor_refusal);
    argument.cache = cache_place;
    spisor_status status = cache->installed(cache->context, &argument, &installed, &declined);
    cJSON *json = status == SPISOR_OK ? host_installed_json(&installed) : NULL;
    *refusal = status == SPISOR_OK ? NULL : host_refusal_json(&declined);
    cache->release_installed_answer(cache->context, &installed);
    cache->release_refusal(cache->context, &declined);
    return json;
}

cJSON *host_damaged_of(const char *cache_place, cJSON **refusal) {
    const runtime_cache_definition *cache = runtime_cache_definition_of();
    runtime_cache_place_argument argument = SPISOR_INIT(runtime_cache_place_argument);
    runtime_cache_damaged_answer damaged = SPISOR_INIT(runtime_cache_damaged_answer);
    spisor_refusal declined = SPISOR_INIT(spisor_refusal);
    argument.cache = cache_place;
    spisor_status status = cache->damaged(cache->context, &argument, &damaged, &declined);
    cJSON *json = status == SPISOR_OK ? host_damaged_json(&damaged) : NULL;
    *refusal = status == SPISOR_OK ? NULL : host_refusal_json(&declined);
    cache->release_damaged_answer(cache->context, &damaged);
    cache->release_refusal(cache->context, &declined);
    return json;
}

cJSON *host_removal_of(const char *state, const char *cache_place, const char *id, cJSON **refusal) {
    const runtime_hosts_definition *hosts = runtime_hosts_definition_of();
    runtime_registry_forget_argument argument = SPISOR_INIT(runtime_registry_forget_argument);
    runtime_hosts_remove_answer removal = SPISOR_INIT(runtime_hosts_remove_answer);
    spisor_refusal declined = SPISOR_INIT(spisor_refusal);
    argument.state = state;
    argument.cache = cache_place;
    argument.id = id;
    spisor_status status = hosts->remove(hosts->context, &argument, &removal, &declined);
    cJSON *json = status == SPISOR_OK ? host_removal_json(&removal) : NULL;
    *refusal = status == SPISOR_OK ? NULL : host_refusal_json(&declined);
    hosts->release_remove_answer(hosts->context, &removal);
    hosts->release_refusal(hosts->context, &declined);
    return json;
}

cJSON *host_sweep_of(const char *cache_place, const char *state, cJSON **refusal) {
    const runtime_cache_definition *cache = runtime_cache_definition_of();
    runtime_cache_sweep_argument argument = SPISOR_INIT(runtime_cache_sweep_argument);
    runtime_cache_sweep_answer sweep = SPISOR_INIT(runtime_cache_sweep_answer);
    spisor_refusal declined = SPISOR_INIT(spisor_refusal);
    argument.cache = cache_place;
    argument.state = state;
    spisor_status status = cache->sweep(cache->context, &argument, &sweep, &declined);
    cJSON *json = status == SPISOR_OK ? host_sweep_json(&sweep) : NULL;
    *refusal = status == SPISOR_OK ? NULL : host_refusal_json(&declined);
    cache->release_sweep_answer(cache->context, &sweep);
    cache->release_refusal(cache->context, &declined);
    return json;
}
