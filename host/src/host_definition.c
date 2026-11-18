#include "host_definition.h"

#include "allocation.h"
#include "json_value.h"

static const char *const START_RUNTIMES[] = {NULL, "javascript", "jvm", "native"};
static const char *const SOURCE_OUTCOMES[] = {NULL, "skipped", "unchanged", "changed", "failed"};
static const char *const APPLICATION_OPERATIONS[] = {NULL, "update", "remove"};

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
            add_text(json, "location", provenance->location) && add_text(json, "os", provenance->os) &&
            add_text(json, "architecture", provenance->architecture) &&
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

static const char *named(const char *const *names, size_t count, uint32_t value) {
    return value < count ? names[value] : NULL;
}

static cJSON *problem_json(const runtime_updates_problem *problem) {
    cJSON *json = cJSON_CreateObject();
    if (json == NULL || !add_text(json, "id", problem->id) || !add_text(json, "message", problem->detail) ||
        !add_text(json, "fix", problem->fix)) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static cJSON *problems_json(const runtime_updates_problem *problems, size_t count) {
    cJSON *json = cJSON_CreateArray();
    int complete = json != NULL;
    for (size_t index = 0; complete && index < count; index++) {
        complete = add_to_array(json, problem_json(&problems[index]));
    }
    if (!complete) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static cJSON *source_check_json(const runtime_updates_source_check *check) {
    cJSON *json = cJSON_CreateObject();
    if (json == NULL || !add_text(json, "kind", named(SOURCE_OUTCOMES, 5, check->outcome)) ||
        !add_text(json, "detail", check->detail)) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static cJSON *host_check_json(const runtime_updates_host_check *check) {
    cJSON *json = cJSON_CreateObject();
    cJSON *checked = json == NULL ? NULL : cJSON_AddArrayToObject(json, "checked");
    int complete = checked != NULL;
    for (size_t index = 0; complete && index < check->source_count; index++) {
        complete = add_to_array(checked, source_check_json(&check->sources[index]));
    }
    complete = complete && add_owned(json, "fetched", texts_json(check->fetched, check->fetched_count)) &&
               add_owned(json, "refused", problems_json(check->refused, check->refused_count)) &&
               add_owned(json, "removed", texts_json(check->removed, check->removed_count));
    if (!complete) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static cJSON *application_json(const runtime_updates_application *application) {
    cJSON *json = cJSON_CreateObject();
    const char *operation = named(APPLICATION_OPERATIONS, 3, application->operation);
    int complete =
        json != NULL && add_text(json, "operation", operation) && add_text(json, "version", application->version);
    if (complete && application->unrecorded_detail != NULL) {
        cJSON *unrecorded = cJSON_AddObjectToObject(json, "unrecorded");
        complete = unrecorded != NULL && add_text(unrecorded, "message", application->unrecorded_detail) &&
                   add_text(unrecorded, "fix", application->unrecorded_fix);
    }
    if (!complete) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static cJSON *run_refusal_json(const runtime_updates_problem *problem) {
    cJSON *json = cJSON_CreateObject();
    if (json == NULL || !add_text(json, "message", problem->detail) || !add_text(json, "fix", problem->fix) ||
        !add_text(json, "id", problem->id)) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static cJSON *run_refused_json(const runtime_updates_problem *problems, size_t count) {
    cJSON *json = cJSON_CreateArray();
    int complete = json != NULL;
    for (size_t index = 0; complete && index < count; index++) {
        complete = add_to_array(json, run_refusal_json(&problems[index]));
    }
    if (!complete) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static cJSON *run_json(const runtime_updates_run_answer *run) {
    cJSON *json = cJSON_CreateObject();
    cJSON *checks = json == NULL ? NULL : cJSON_AddObjectToObject(json, "checks");
    cJSON *applications = checks == NULL ? NULL : cJSON_AddObjectToObject(json, "applications");
    int complete = applications != NULL;
    for (size_t index = 0; complete && index < run->check_count; index++) {
        complete = add_owned(checks, run->checks[index].host_id, host_check_json(&run->checks[index]));
    }
    for (size_t index = 0; complete && index < run->application_count; index++) {
        const runtime_updates_application *application = &run->applications[index];
        complete = add_owned(applications, application->loader, application_json(application));
    }
    if (!complete || !add_owned(json, "refused", run_refused_json(run->refused, run->refused_count))) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

cJSON *host_update_run(const char *portable, const char *state, const char *cache_place, const char *now,
                       cJSON **refusal) {
    const runtime_updates_definition *updates = runtime_updates_definition_of();
    runtime_updates_run_argument argument = SPISOR_INIT(runtime_updates_run_argument);
    runtime_updates_run_answer run = SPISOR_INIT(runtime_updates_run_answer);
    spisor_refusal declined = SPISOR_INIT(spisor_refusal);
    argument.portable = portable;
    argument.state = state;
    argument.cache = cache_place;
    argument.now = now;
    spisor_status status = updates->run(updates->context, &argument, &run, &declined);
    cJSON *json = status == SPISOR_OK ? run_json(&run) : NULL;
    *refusal = status == SPISOR_OK ? NULL : host_refusal_json(&declined);
    updates->release_run_answer(updates->context, &run);
    updates->release_refusal(updates->context, &declined);
    return json;
}

runtime_variable *host_variables_of(const cJSON *object, size_t *count) {
    size_t capacity = (size_t)cJSON_GetArraySize(object);
    runtime_variable *variables = spisor_memory_allocate((capacity == 0 ? 1 : capacity) * sizeof *variables);
    const cJSON *listed = variables == NULL ? NULL : object;
    const cJSON *member;
    *count = 0;
    cJSON_ArrayForEach(member, listed) {
        if (cJSON_IsString(member)) {
            variables[*count].name = member->string;
            variables[*count].value = member->valuestring;
            (*count)++;
        }
    }
    return variables;
}

const char **host_texts_of(const cJSON *array, size_t *count) {
    size_t capacity = (size_t)cJSON_GetArraySize(array);
    const char **texts = spisor_memory_allocate((capacity == 0 ? 1 : capacity) * sizeof *texts);
    const cJSON *listed = texts == NULL ? NULL : array;
    const cJSON *element;
    *count = 0;
    cJSON_ArrayForEach(element, listed) {
        if (cJSON_IsString(element)) {
            texts[(*count)++] = element->valuestring;
        }
    }
    return texts;
}

cJSON *host_variables_json(const runtime_variable *variables, size_t count) {
    cJSON *json = cJSON_CreateObject();
    for (size_t index = 0; json != NULL && index < count; index++) {
        if (!cJSON_HasObjectItem(json, variables[index].name) &&
            cJSON_AddStringToObject(json, variables[index].name, variables[index].value) == NULL) {
            cJSON_Delete(json);
            json = NULL;
        }
    }
    return json;
}
