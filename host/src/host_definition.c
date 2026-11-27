#include "host_definition.h"

#include <string.h>

#include "allocation.h"
#include "spisor/json_value.h"

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

static const char *const APPLICATION_STATES[] = {NULL, "installed", "detected", "missing", "skipped", "failed",
                                                 "unavailable"};
static const char *const JAVASCRIPT_SOURCES[] = {NULL, "path", "acquired"};

static cJSON *unavailable_json(const runtime_install_unavailable *rows, size_t count, const char *id_name) {
    cJSON *json = cJSON_CreateArray();
    int complete = json != NULL;
    for (size_t index = 0; complete && index < count; index++) {
        cJSON *row = cJSON_CreateObject();
        complete = row != NULL && add_text(row, id_name, rows[index].id) &&
                   add_text(row, "message", rows[index].detail) && add_text(row, "fix", rows[index].fix);
        if (!complete) {
            cJSON_Delete(row);
            row = NULL;
        }
        complete = add_to_array(json, row);
    }
    if (!complete) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static cJSON *item_versions_json(const runtime_item_version *versions, size_t count) {
    cJSON *json = cJSON_CreateArray();
    int complete = json != NULL;
    for (size_t index = 0; complete && index < count; index++) {
        cJSON *row = cJSON_CreateObject();
        complete = row != NULL && add_text(row, "id", versions[index].id) &&
                   add_text(row, "version", versions[index].version);
        if (!complete) {
            cJSON_Delete(row);
            row = NULL;
        }
        complete = add_to_array(json, row);
    }
    if (!complete) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static int add_origin(cJSON *row, const runtime_install_origin *origin) {
    const runtime_install_package *package = &origin->package;
    if (package->method == NULL && package->name == NULL && package->options == NULL && origin->version == NULL) {
        return 1;
    }
    cJSON *json = cJSON_AddObjectToObject(row, "origin");
    return json != NULL && add_text(json, "method", package->method) && add_text(json, "package", package->name) &&
           add_value(json, "options", package->options) && add_text(json, "version", origin->version);
}

static cJSON *application_row_json(const runtime_install_application *application) {
    cJSON *json = cJSON_CreateObject();
    int complete = json != NULL && add_text(json, "id", application->id) &&
                   add_text(json, "state", named(APPLICATION_STATES, 7, application->state)) &&
                   add_text(json, "installedThrough", application->installed_through) &&
                   add_origin(json, &application->origin) &&
                   (application->offered_count == 0 ||
                    add_owned(json, "offered", texts_json(application->offered, application->offered_count))) &&
                   add_text(json, "flag", application->flag) &&
                   (application->unavailable_count == 0 ||
                    add_owned(json, "unavailable",
                              unavailable_json(application->unavailable, application->unavailable_count, "method")));
    if (!complete) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static cJSON *refused_commands_json(const runtime_install_refused_command *refused, size_t count) {
    cJSON *json = cJSON_CreateArray();
    int complete = json != NULL;
    for (size_t index = 0; complete && index < count; index++) {
        cJSON *row = cJSON_CreateObject();
        complete = row != NULL && add_text(row, "name", refused[index].name) &&
                   add_text(row, "message", refused[index].detail) && add_text(row, "fix", refused[index].fix);
        if (!complete) {
            cJSON_Delete(row);
            row = NULL;
        }
        complete = add_to_array(json, row);
    }
    if (!complete) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static int add_entry(cJSON *row, const runtime_install_loader *loader) {
    cJSON *entry = cJSON_AddObjectToObject(row, "entry");
    return entry != NULL && cJSON_AddBoolToObject(entry, "written", loader->entry_written != 0) != NULL &&
           (!loader->entry_runs_in_process || cJSON_AddTrueToObject(entry, "runsInProcess") != NULL) &&
           add_text(entry, "message", loader->entry_detail) && add_text(entry, "fix", loader->entry_fix);
}

static cJSON *loader_row_json(const runtime_install_loader *loader) {
    cJSON *json = cJSON_CreateObject();
    int complete = json != NULL && add_text(json, "id", loader->id) &&
                   add_text(json, "application", loader->application) && add_text(json, "home", loader->home) &&
                   add_owned(json, "commands", texts_json(loader->commands, loader->command_count)) &&
                   add_owned(json, "refusedCommands",
                             refused_commands_json(loader->refused_commands, loader->refused_command_count)) &&
                   add_entry(json, loader);
    if (!complete) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static cJSON *missing_row_json(const runtime_install_missing *missing) {
    cJSON *json = cJSON_CreateObject();
    if (json == NULL || !add_text(json, "application", missing->application) ||
        !add_owned(json, "methods", texts_json(missing->methods, missing->method_count))) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static cJSON *javascript_json(const runtime_install_javascript *javascript) {
    cJSON *json = cJSON_CreateObject();
    if (json == NULL || !add_text(json, "name", javascript->name) || !add_text(json, "version", javascript->version) ||
        !add_text(json, "executable", javascript->executable) ||
        cJSON_AddBoolToObject(json, "fetched", javascript->fetched != 0) == NULL ||
        !add_text(json, "source", named(JAVASCRIPT_SOURCES, 3, javascript->source))) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

/** Adds an array under name, each of `count` rows of `width` bytes written by `row_json`; 0 when memory runs out. */
static int add_rows(cJSON *object, const char *name, const void *rows, size_t count, size_t width,
                    cJSON *(*row_json)(const void *row)) {
    cJSON *array = cJSON_AddArrayToObject(object, name);
    int complete = array != NULL;
    for (size_t index = 0; complete && index < count; index++) {
        complete = add_to_array(array, row_json((const char *)rows + index * width));
    }
    return complete;
}

static cJSON *any_application_row(const void *row) {
    return application_row_json(row);
}

static cJSON *any_loader_row(const void *row) {
    return loader_row_json(row);
}

static cJSON *any_missing_row(const void *row) {
    return missing_row_json(row);
}

static cJSON *ecosystem_json(const runtime_install_ecosystem_answer *report) {
    cJSON *json = cJSON_CreateObject();
    int complete =
        json != NULL && add_owned(json, "installed", item_versions_json(report->installed, report->installed_count)) &&
        add_owned(json, "cached", texts_json(report->cached, report->cached_count)) &&
        add_owned(json, "unavailable", unavailable_json(report->unavailable, report->unavailable_count, "id")) &&
        add_owned(json, "enabled", texts_json(report->enabled, report->enabled_count)) &&
        add_rows(json, "applications", report->applications, report->application_count,
                 sizeof *report->applications, any_application_row) &&
        add_rows(json, "loaders", report->loaders, report->loader_count, sizeof *report->loaders, any_loader_row) &&
        add_rows(json, "missing", report->missing, report->missing_count, sizeof *report->missing,
                 any_missing_row) &&
        (report->javascript == NULL || add_owned(json, "javascript", javascript_json(report->javascript)));
    if (!complete) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

/** The --catalog sources, {kind: local, path} or {kind: https, url}, as catalog sources; freed, NULL out of memory. */
static runtime_catalog_source *catalog_sources_of(const cJSON *sources, size_t *count) {
    size_t capacity = (size_t)cJSON_GetArraySize(sources);
    runtime_catalog_source *listed = spisor_memory_allocate((capacity == 0 ? 1 : capacity) * sizeof *listed);
    const cJSON *each = listed == NULL ? NULL : sources;
    const cJSON *source;
    *count = 0;
    cJSON_ArrayForEach(source, each) {
        const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(source, "kind"));
        int is_https = kind != NULL && strcmp(kind, "https") == 0;
        listed[*count].kind = is_https ? RUNTIME_CATALOG_HTTPS : RUNTIME_CATALOG_LOCAL;
        const cJSON *location = cJSON_GetObjectItemCaseSensitive(source, is_https ? "url" : "path");
        listed[*count].location = cJSON_GetStringValue(location);
        (*count)++;
    }
    return listed;
}

static runtime_install_choice *choices_of(const cJSON *choices, size_t *count) {
    size_t capacity = (size_t)cJSON_GetArraySize(choices);
    runtime_install_choice *listed = spisor_memory_allocate((capacity == 0 ? 1 : capacity) * sizeof *listed);
    const cJSON *each = listed == NULL ? NULL : choices;
    const cJSON *choice;
    *count = 0;
    cJSON_ArrayForEach(choice, each) {
        listed[*count].application = choice->string;
        listed[*count].method = choice->valuestring;
        (*count)++;
    }
    return listed;
}

/** The parts of an ecosystem argument the host holds as JSON, borrowed from it; each freed by release_parts. */
typedef struct ecosystem_parts {
    runtime_catalog_source *sources;
    runtime_variable *environment;
    runtime_install_choice *choices;
    const char **requested;
} ecosystem_parts;

static int take_parts(const host_ecosystem_call *call, ecosystem_parts *parts,
                      runtime_install_ecosystem_argument *argument) {
    parts->sources = catalog_sources_of(call->sources, &argument->source_count);
    parts->environment = host_variables_of(call->environment, &argument->environment_count);
    parts->choices = choices_of(call->choices, &argument->choice_count);
    parts->requested = host_texts_of(call->requested, &argument->requested_count);
    argument->sources = parts->sources;
    argument->environment = parts->environment;
    argument->choices = parts->choices;
    argument->requested = parts->requested;
    return parts->sources != NULL && parts->environment != NULL && parts->choices != NULL && parts->requested != NULL;
}

static void release_parts(ecosystem_parts *parts) {
    spisor_memory_release(parts->sources);
    spisor_memory_release(parts->environment);
    spisor_memory_release(parts->choices);
    spisor_memory_release(parts->requested);
}

cJSON *host_install_ecosystem(const host_ecosystem_call *call, cJSON **refusal) {
    const runtime_install_definition *install = runtime_install_definition_of();
    runtime_install_ecosystem_argument argument = SPISOR_INIT(runtime_install_ecosystem_argument);
    runtime_install_ecosystem_answer report = SPISOR_INIT(runtime_install_ecosystem_answer);
    spisor_refusal declined = SPISOR_INIT(spisor_refusal);
    ecosystem_parts parts = {NULL, NULL, NULL, NULL};
    argument.portable = call->portable;
    argument.state = call->state;
    argument.cache = call->cache;
    spisor_status status = take_parts(call, &parts, &argument) ? SPISOR_OK : SPISOR_OUT_OF_MEMORY;
    if (status == SPISOR_OK) {
        status = install->ecosystem(install->context, &argument, &report, &declined);
    }
    cJSON *json = status == SPISOR_OK ? ecosystem_json(&report) : NULL;
    *refusal = status == SPISOR_OK ? NULL : host_refusal_json(&declined);
    install->release_ecosystem_answer(install->context, &report);
    install->release_refusal(install->context, &declined);
    release_parts(&parts);
    return json;
}

int host_place_boot(const char *cache_place, cJSON **refusal) {
    const runtime_boot_definition *boot = runtime_boot_definition_of();
    runtime_boot_place_argument argument = SPISOR_INIT(runtime_boot_place_argument);
    runtime_boot_place_answer placed = SPISOR_INIT(runtime_boot_place_answer);
    spisor_refusal declined = SPISOR_INIT(spisor_refusal);
    argument.cache = cache_place;
    spisor_status status = boot->place(boot->context, &argument, &placed, &declined);
    *refusal = status == SPISOR_OK ? NULL : host_refusal_json(&declined);
    boot->release_place_answer(boot->context, &placed);
    boot->release_refusal(boot->context, &declined);
    return status == SPISOR_OK;
}
