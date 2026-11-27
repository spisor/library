#ifndef RUNTIME_TEST_INSTALL_CALL_H
#define RUNTIME_TEST_INSTALL_CALL_H

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "cache_call.h"
#include "catalog_call.h"
#include "catalog_service_call.h"
#include "check.h"
#include "definition_support.h"
#include "runtime/install.h"
#include "spisor/status.h"
#include "spisor/value.h"

/**
 * @file
 * Calls the install definition with a request a test writes as JSON, in the shape install's operations take inside
 * runtime, and answers what the definition answered as that operation's JSON. A refusal answers {message, fix} with
 * the definition's status. Test code only: the JSON is the test's data, never the boundary.
 */

typedef enum install_operation {
    INSTALL_WARNINGS,
    INSTALL_PACKAGES,
    INSTALL_PLAN,
    INSTALL_DETECT,
    INSTALL_ENTRY,
    INSTALL_RUNTIME,
    INSTALL_APPLICATION,
    INSTALL_REGISTER_LOADER,
    INSTALL_BY_ID,
    INSTALL_ECOSYSTEM
} install_operation;

static inline const char *install_place_of(const cJSON *request, const char *name) {
    return cache_member_text(cJSON_GetObjectItemCaseSensitive(request, "places"), name);
}

/** The texts of a JSON array of strings, a non-string item read as NULL; NULL for an absent array. */
static inline const char *const *install_texts_of(catalog_request *held, const cJSON *array, size_t *count) {
    const char **texts;
    const cJSON *entry;
    *count = 0;
    if (array == NULL) {
        return NULL;
    }
    texts = catalog_request_block(held, (size_t)cJSON_GetArraySize(array), sizeof(const char *));
    cJSON_ArrayForEach(entry, array) {
        texts[(*count)++] = cJSON_GetStringValue(entry);
    }
    return texts;
}

static inline void install_add_value(cJSON *object, const char *name, const spisor_value *value) {
    if (value != NULL) {
        CHECK(cJSON_AddItemToObject(object, name, catalog_record_json(value)));
    }
}

/** A package as install's operations answer one, {name, options?}. */
static inline cJSON *install_package_json(const runtime_install_package *package) {
    cJSON *json = cJSON_CreateObject();
    cache_add_text(json, "name", package->name);
    install_add_value(json, "options", package->options);
    return json;
}

static inline spisor_status install_warnings_call(const runtime_install_definition *install, const cJSON *request,
                                                  catalog_request *held, cJSON **tree, spisor_refusal *refusal) {
    static const char *const kinds[] = {"", "named-dependency", "unbound-service"};
    runtime_install_warnings_argument argument = SPISOR_INIT(runtime_install_warnings_argument);
    runtime_install_warnings_answer answer = SPISOR_INIT(runtime_install_warnings_answer);
    spisor_status status;
    argument.manifest = catalog_request_value(held, request, "manifest");
    status = install->warnings(install->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        cJSON *warnings = cJSON_CreateArray();
        for (size_t index = 0; index < answer.warning_count; index++) {
            cJSON *each = cJSON_CreateObject();
            CHECK(answer.warnings[index].kind <= RUNTIME_INSTALL_UNBOUND_SERVICE);
            cache_add_text(each, "kind", kinds[answer.warnings[index].kind]);
            cache_add_text(each, "subject", answer.warnings[index].subject);
            cache_add_text(each, "detail", answer.warnings[index].detail);
            CHECK(cJSON_AddItemToArray(warnings, each));
        }
        *tree = cJSON_CreateObject();
        CHECK(cJSON_AddItemToObject(*tree, "warnings", warnings));
    }
    install->release_warnings_answer(install->context, &answer);
    return status;
}

static inline spisor_status install_packages_call(const runtime_install_definition *install, const cJSON *request,
                                                  catalog_request *held, cJSON **tree, spisor_refusal *refusal) {
    runtime_install_packages_argument argument = SPISOR_INIT(runtime_install_packages_argument);
    runtime_install_packages_answer answer = SPISOR_INIT(runtime_install_packages_answer);
    spisor_status status;
    argument.portable = install_place_of(request, "portable");
    argument.item = cache_member_text(request, "item");
    argument.manifest_packages = catalog_request_value(held, request, "manifestPackages");
    status = install->packages(install->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        cJSON *packages = cJSON_CreateObject();
        for (size_t index = 0; index < answer.package_count; index++) {
            CHECK(cJSON_AddItemToObject(packages, answer.packages[index].method,
                                        install_package_json(&answer.packages[index])));
        }
        *tree = cJSON_CreateObject();
        CHECK(cJSON_AddItemToObject(*tree, "packages", packages));
    }
    install->release_packages_answer(install->context, &answer);
    return status;
}

static inline cJSON *install_step_json(const runtime_install_step *step) {
    cJSON *json = cJSON_CreateObject();
    if (step->kind == RUNTIME_INSTALL_PROVIDER_STEP) {
        install_add_value(json, "install-provider", step->provider);
    } else {
        cJSON *detail = cJSON_CreateObject();
        cache_add_text(detail, "method", step->package.method);
        if (step->package.name != NULL) {
            CHECK(cJSON_AddItemToObject(detail, "package", install_package_json(&step->package)));
        }
        CHECK(cJSON_AddItemToObject(json, "install", detail));
    }
    return json;
}

static inline spisor_status install_plan_call(const runtime_install_definition *install, const cJSON *request,
                                              catalog_request *held, cJSON **tree, spisor_refusal *refusal) {
    runtime_install_plan_argument argument = SPISOR_INIT(runtime_install_plan_argument);
    runtime_install_plan_answer answer = SPISOR_INIT(runtime_install_plan_answer);
    spisor_status status;
    argument.portable = install_place_of(request, "portable");
    argument.item = cache_member_text(request, "item");
    argument.manifest_packages = catalog_request_value(held, request, "manifestPackages");
    argument.methods = install_texts_of(held, cJSON_GetObjectItemCaseSensitive(request, "methods"),
                                        &argument.method_count);
    argument.chosen = cache_member_text(request, "chosen");
    argument.installed_providers = install_texts_of(
        held, cJSON_GetObjectItemCaseSensitive(request, "installedProviders"), &argument.installed_provider_count);
    argument.catalog = catalog_request_value(held, request, "catalog");
    status = install->plan(install->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        cJSON *steps = cJSON_CreateArray();
        for (size_t index = 0; index < answer.step_count; index++) {
            CHECK(cJSON_AddItemToArray(steps, install_step_json(&answer.steps[index])));
        }
        *tree = cJSON_CreateObject();
        CHECK(cJSON_AddItemToObject(*tree, "steps", steps));
    }
    install->release_plan_answer(install->context, &answer);
    return status;
}

static inline cJSON *install_detection_json(const runtime_install_detection *detection) {
    cJSON *json = cJSON_CreateObject();
    cache_add_text(json, "method", detection->package.method);
    cache_add_text(json, "item", detection->item);
    cache_add_text(json, "package", detection->package.name);
    install_add_value(json, "options", detection->package.options);
    if (detection->refused != NULL) {
        cache_add_text(json, "refused", detection->refused);
    } else {
        cJSON *found = cJSON_CreateObject();
        CHECK(cJSON_AddBoolToObject(found, "installed", detection->installed != 0) != NULL);
        cache_add_text(found, "version", detection->version);
        CHECK(cJSON_AddItemToObject(json, "answer", found));
    }
    return json;
}

static inline spisor_status install_detect_call(const runtime_install_definition *install, const cJSON *request,
                                                catalog_request *held, cJSON **tree, spisor_refusal *refusal) {
    runtime_install_detect_argument argument = SPISOR_INIT(runtime_install_detect_argument);
    runtime_install_detect_answer answer = SPISOR_INIT(runtime_install_detect_answer);
    spisor_status status;
    argument.portable = install_place_of(request, "portable");
    argument.cache = install_place_of(request, "cache");
    argument.item = cache_member_text(request, "item");
    argument.manifest_packages = catalog_request_value(held, request, "manifestPackages");
    status = install->detect(install->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        cJSON *detections = cJSON_CreateArray();
        for (size_t index = 0; index < answer.detection_count; index++) {
            CHECK(cJSON_AddItemToArray(detections, install_detection_json(&answer.detections[index])));
        }
        *tree = cJSON_CreateObject();
        CHECK(cJSON_AddItemToObject(*tree, "detections", detections));
    }
    install->release_detect_answer(install->context, &answer);
    return status;
}

/** The request's environment object as variables, held until the call's answer is turned into JSON. */
static inline const runtime_variable *install_environment_of(catalog_request *held, const cJSON *request,
                                                             size_t *count) {
    runtime_variable *variables =
        runtime_variables_of_json(cJSON_GetObjectItemCaseSensitive(request, "environment"), count);
    CHECK(held->block_count < 8);
    held->blocks[held->block_count++] = variables;
    return variables;
}

static inline spisor_status install_entry_call(const runtime_install_definition *install, const cJSON *request,
                                               catalog_request *held, cJSON **tree, spisor_refusal *refusal) {
    const cJSON *loader = cJSON_GetObjectItemCaseSensitive(request, "loader");
    runtime_install_entry_argument argument = SPISOR_INIT(runtime_install_entry_argument);
    runtime_install_entry_answer answer = SPISOR_INIT(runtime_install_entry_answer);
    spisor_status status;
    argument.state = install_place_of(request, "state");
    argument.cache = install_place_of(request, "cache");
    argument.loader = cache_member_text(loader, "id");
    argument.home = cache_member_text(loader, "home");
    argument.application = catalog_request_value(held, loader, "application");
    argument.language = cache_member_text(loader, "language");
    status = install->entry(install->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        *tree = cJSON_CreateObject();
        CHECK(cJSON_AddBoolToObject(*tree, "written", answer.written != 0) != NULL);
        if (answer.runs_in_process) {
            CHECK(cJSON_AddTrueToObject(*tree, "runsInProcess") != NULL);
        }
        if (answer.written) {
            cJSON *sequences = cJSON_AddArrayToObject(*tree, "sequences");
            for (size_t index = 0; index < answer.sequence_count; index++) {
                CHECK(cJSON_AddItemToArray(sequences, cJSON_CreateNumber((double)answer.sequences[index])));
            }
        }
        cache_add_text(*tree, "message", answer.detail);
        cache_add_text(*tree, "fix", answer.fix);
    }
    install->release_entry_answer(install->context, &answer);
    return status;
}

static inline cJSON *install_javascript_json(const runtime_install_javascript *javascript) {
    cJSON *json = cJSON_CreateObject();
    cache_add_text(json, "name", javascript->name);
    cache_add_text(json, "version", javascript->version);
    cache_add_text(json, "executable", javascript->executable);
    CHECK(cJSON_AddBoolToObject(json, "fetched", javascript->fetched != 0) != NULL);
    cache_add_text(json, "source", javascript->source == RUNTIME_INSTALL_FROM_PATH ? "path" : "acquired");
    return json;
}

static inline spisor_status install_runtime_call(const runtime_install_definition *install, const cJSON *request,
                                                 catalog_request *held, cJSON **tree, spisor_refusal *refusal) {
    runtime_install_runtime_argument argument = SPISOR_INIT(runtime_install_runtime_argument);
    runtime_install_runtime_answer answer = SPISOR_INIT(runtime_install_runtime_answer);
    spisor_status status;
    argument.cache = cache_member_text(request, "cache");
    argument.platform = cache_member_text(request, "platform");
    argument.environment = install_environment_of(held, request, &argument.environment_count);
    status = install->runtime(install->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        *tree = install_javascript_json(&answer.javascript);
    }
    install->release_runtime_answer(install->context, &answer);
    return status;
}

static inline spisor_status install_application_call(const runtime_install_definition *install,
                                                     const cJSON *request, catalog_request *held, cJSON **tree,
                                                     spisor_refusal *refusal) {
    const char *operation = cache_member_text(request, "operation");
    runtime_install_application_argument argument = SPISOR_INIT(runtime_install_application_argument);
    runtime_install_application_answer answer = SPISOR_INIT(runtime_install_application_answer);
    spisor_status status;
    (void)held;
    argument.portable = install_place_of(request, "portable");
    argument.state = install_place_of(request, "state");
    argument.cache = install_place_of(request, "cache");
    argument.loader = cache_member_text(request, "loader");
    argument.operation = operation != NULL && strcmp(operation, "update") == 0   ? RUNTIME_INSTALL_UPDATE
                         : operation != NULL && strcmp(operation, "remove") == 0 ? RUNTIME_INSTALL_REMOVE
                                                                                 : 0u;
    status = install->application(install->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        *tree = cJSON_CreateObject();
        cache_add_text(*tree, "operation", answer.operation == RUNTIME_INSTALL_UPDATE ? "update" : "remove");
        cache_add_text(*tree, "version", answer.version);
        if (answer.unrecorded_detail != NULL) {
            cJSON *unrecorded = cJSON_AddObjectToObject(*tree, "unrecorded");
            cache_add_text(unrecorded, "message", answer.unrecorded_detail);
            cache_add_text(unrecorded, "fix", answer.unrecorded_fix);
        }
    }
    install->release_application_answer(install->context, &answer);
    return status;
}

static inline cJSON *install_provenance_json(const runtime_registry_provenance *provenance) {
    cJSON *json = cJSON_CreateObject();
    cache_add_text(json, "method", provenance->method);
    cache_add_text(json, "package", provenance->package);
    install_add_value(json, "options", provenance->options);
    cache_add_text(json, "version", provenance->version);
    cache_add_text(json, "location", provenance->location);
    cache_add_text(json, "os", provenance->os);
    cache_add_text(json, "architecture", provenance->architecture);
    cache_add_text(json, "installedAt", provenance->installed_at);
    return json;
}

static inline cJSON *install_refused_commands_json(const runtime_install_refused_command *commands, size_t count) {
    cJSON *array = cJSON_CreateArray();
    for (size_t index = 0; index < count; index++) {
        cJSON *each = cJSON_CreateObject();
        cache_add_text(each, "name", commands[index].name);
        cache_add_text(each, "detail", commands[index].detail);
        cache_add_text(each, "fix", commands[index].fix);
        CHECK(cJSON_AddItemToArray(array, each));
    }
    return array;
}

static inline spisor_status install_register_loader_call(const runtime_install_definition *install,
                                                         const cJSON *request, catalog_request *held, cJSON **tree,
                                                         spisor_refusal *refusal) {
    runtime_install_register_loader_argument argument = SPISOR_INIT(runtime_install_register_loader_argument);
    runtime_install_register_loader_answer answer = SPISOR_INIT(runtime_install_register_loader_answer);
    spisor_status status;
    argument.portable = install_place_of(request, "portable");
    argument.state = install_place_of(request, "state");
    argument.cache = install_place_of(request, "cache");
    argument.environment = install_environment_of(held, request, &argument.environment_count);
    argument.id = cache_member_text(request, "id");
    argument.version = cache_member_text(request, "version");
    status = install->register_loader(install->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        *tree = cJSON_CreateObject();
        if (answer.provenance != NULL) {
            CHECK(cJSON_AddItemToObject(*tree, "provenance", install_provenance_json(answer.provenance)));
        }
        CHECK(cJSON_AddItemToObject(*tree, "commands", cache_texts_json(answer.commands, answer.command_count)));
        CHECK(cJSON_AddItemToObject(*tree, "refusedCommands", install_refused_commands_json(
                                                                  answer.refused_commands, answer.refused_command_count)));
    }
    install->release_register_loader_answer(install->context, &answer);
    return status;
}

static inline spisor_status install_by_id_call(const runtime_install_definition *install, const cJSON *request,
                                               catalog_request *held, cJSON **tree, spisor_refusal *refusal) {
    runtime_install_by_id_argument argument = SPISOR_INIT(runtime_install_by_id_argument);
    runtime_install_by_id_answer answer = SPISOR_INIT(runtime_install_by_id_answer);
    spisor_status status;
    (void)held;
    argument.id = cache_member_text(request, "id");
    argument.cache = cache_member_text(request, "cache");
    argument.plugin_directory = cache_member_text(request, "pluginDir");
    argument.version = cache_member_text(request, "version");
    argument.last_known = cache_member_text(request, "lastKnown");
    argument.accept_primary = cache_member_text(request, "acceptPrimary");
    argument.api_base = cache_member_text(request, "apiBase");
    status = install->by_id(install->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        *tree = cJSON_CreateObject();
        cache_add_text(*tree, "id", answer.id);
        cache_add_text(*tree, "name", answer.name);
        cache_add_text(*tree, "fullName", answer.full_name);
        CHECK(cJSON_AddItemToObject(*tree, "ids", cache_texts_json(answer.ids, answer.id_count)));
        cache_add_text(*tree, "primary", answer.primary);
        CHECK(cJSON_AddBoolToObject(*tree, "primaryConfirmed", answer.primary_confirmed != 0) != NULL);
        if (answer.needs_user_acceptance) {
            cache_add_text(*tree, "recordedPrimary", answer.recorded_primary);
            CHECK(cJSON_AddTrueToObject(*tree, "needsUserAcceptance") != NULL);
        }
        cache_add_text(*tree, "version", answer.version);
        cache_add_text(*tree, "commit", answer.commit);
        cache_add_text(*tree, "sha256", answer.sha256);
    }
    install->release_by_id_answer(install->context, &answer);
    return status;
}

/** The request's choices object as choices, held until the call's answer is turned into JSON. */
static inline const runtime_install_choice *install_choices_of(catalog_request *held, const cJSON *choices,
                                                               size_t *count) {
    runtime_install_choice *listed;
    const cJSON *choice;
    *count = 0;
    if (choices == NULL) {
        return NULL;
    }
    listed = catalog_request_block(held, (size_t)cJSON_GetArraySize(choices), sizeof *listed);
    cJSON_ArrayForEach(choice, choices) {
        listed[*count].application = choice->string;
        listed[(*count)++].method = cJSON_GetStringValue(choice);
    }
    return listed;
}

static inline cJSON *install_unavailable_json(const runtime_install_unavailable *rows, size_t count,
                                              const char *id_name) {
    cJSON *array = cJSON_CreateArray();
    for (size_t index = 0; index < count; index++) {
        cJSON *row = cJSON_CreateObject();
        cache_add_text(row, id_name, rows[index].id);
        cache_add_text(row, "message", rows[index].detail);
        cache_add_text(row, "fix", rows[index].fix);
        CHECK(cJSON_AddItemToArray(array, row));
    }
    return array;
}

static inline cJSON *install_settled_application_json(const runtime_install_application *application) {
    static const char *const states[] = {"", "installed", "detected", "missing", "skipped", "failed", "unavailable"};
    cJSON *json = cJSON_CreateObject();
    cache_add_text(json, "id", application->id);
    cache_add_text(json, "state", application->state < 7 ? states[application->state] : "");
    cache_add_text(json, "installedThrough", application->installed_through);
    if (application->origin.package.method != NULL) {
        cJSON *origin = cJSON_AddObjectToObject(json, "origin");
        cache_add_text(origin, "method", application->origin.package.method);
        cache_add_text(origin, "package", application->origin.package.name);
        install_add_value(origin, "options", application->origin.package.options);
        cache_add_text(origin, "version", application->origin.version);
    }
    if (application->offered != NULL) {
        CHECK(cJSON_AddItemToObject(json, "offered", cache_texts_json(application->offered, application->offered_count)));
    }
    cache_add_text(json, "flag", application->flag);
    if (application->unavailable_count > 0) {
        CHECK(cJSON_AddItemToObject(json, "unavailable", install_unavailable_json(application->unavailable,
                                                                                  application->unavailable_count,
                                                                                  "method")));
    }
    return json;
}

static inline cJSON *install_loader_json(const runtime_install_loader *loader) {
    cJSON *json = cJSON_CreateObject();
    cJSON *entry;
    cache_add_text(json, "id", loader->id);
    cache_add_text(json, "application", loader->application);
    cache_add_text(json, "home", loader->home);
    CHECK(cJSON_AddItemToObject(json, "commands", cache_texts_json(loader->commands, loader->command_count)));
    CHECK(cJSON_AddItemToObject(json, "refusedCommands", install_refused_commands_json(loader->refused_commands,
                                                                                        loader->refused_command_count)));
    entry = cJSON_AddObjectToObject(json, "entry");
    CHECK(cJSON_AddBoolToObject(entry, "written", loader->entry_written != 0) != NULL);
    if (loader->entry_runs_in_process) {
        CHECK(cJSON_AddTrueToObject(entry, "runsInProcess") != NULL);
    }
    cache_add_text(entry, "message", loader->entry_detail);
    cache_add_text(entry, "fix", loader->entry_fix);
    return json;
}

static inline cJSON *install_report_rows_json(const runtime_install_ecosystem_answer *answer) {
    cJSON *report = cJSON_CreateObject();
    cJSON *installed = cJSON_AddArrayToObject(report, "installed");
    cJSON *applications;
    cJSON *loaders;
    cJSON *missing;
    for (size_t index = 0; index < answer->installed_count; index++) {
        cJSON *row = cJSON_CreateObject();
        cache_add_text(row, "id", answer->installed[index].id);
        cache_add_text(row, "version", answer->installed[index].version);
        CHECK(cJSON_AddItemToArray(installed, row));
    }
    CHECK(cJSON_AddItemToObject(report, "cached", cache_texts_json(answer->cached, answer->cached_count)));
    CHECK(cJSON_AddItemToObject(report, "unavailable",
                                install_unavailable_json(answer->unavailable, answer->unavailable_count, "id")));
    CHECK(cJSON_AddItemToObject(report, "enabled", cache_texts_json(answer->enabled, answer->enabled_count)));
    applications = cJSON_AddArrayToObject(report, "applications");
    for (size_t index = 0; index < answer->application_count; index++) {
        CHECK(cJSON_AddItemToArray(applications, install_settled_application_json(&answer->applications[index])));
    }
    loaders = cJSON_AddArrayToObject(report, "loaders");
    for (size_t index = 0; index < answer->loader_count; index++) {
        CHECK(cJSON_AddItemToArray(loaders, install_loader_json(&answer->loaders[index])));
    }
    missing = cJSON_AddArrayToObject(report, "missing");
    for (size_t index = 0; index < answer->missing_count; index++) {
        cJSON *row = cJSON_CreateObject();
        cache_add_text(row, "application", answer->missing[index].application);
        CHECK(cJSON_AddItemToObject(row, "methods", cache_texts_json(answer->missing[index].methods,
                                                                     answer->missing[index].method_count)));
        CHECK(cJSON_AddItemToArray(missing, row));
    }
    return report;
}

static inline spisor_status install_ecosystem_call(const runtime_install_definition *install, const cJSON *request,
                                                   catalog_request *held, cJSON **tree, spisor_refusal *refusal) {
    runtime_install_ecosystem_argument argument = SPISOR_INIT(runtime_install_ecosystem_argument);
    runtime_install_ecosystem_answer answer = SPISOR_INIT(runtime_install_ecosystem_answer);
    spisor_status status;
    argument.sources = catalog_sources_of(held, cJSON_GetObjectItemCaseSensitive(request, "sources"),
                                          &argument.source_count);
    argument.portable = install_place_of(request, "portable");
    argument.state = install_place_of(request, "state");
    argument.cache = install_place_of(request, "cache");
    argument.environment = install_environment_of(held, request, &argument.environment_count);
    argument.choices = install_choices_of(held, cJSON_GetObjectItemCaseSensitive(request, "choices"),
                                          &argument.choice_count);
    argument.defaults = install_texts_of(held, cJSON_GetObjectItemCaseSensitive(request, "defaults"),
                                         &argument.default_count);
    argument.requested = install_texts_of(held, cJSON_GetObjectItemCaseSensitive(request, "requested"),
                                          &argument.requested_count);
    argument.platform = cache_member_text(request, "platform");
    status = install->ecosystem(install->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        *tree = install_report_rows_json(&answer);
        if (answer.javascript != NULL) {
            CHECK(cJSON_AddItemToObject(*tree, "javascript", install_javascript_json(answer.javascript)));
        }
    }
    install->release_ecosystem_answer(install->context, &answer);
    return status;
}

/** Calls the operation with the request, which it deletes, answering its status and its answer or refusal as JSON. */
static inline catalog_answer install_call(install_operation operation, cJSON *request) {
    typedef spisor_status (*caller)(const runtime_install_definition *, const cJSON *, catalog_request *, cJSON **,
                                    spisor_refusal *);
    static const caller callers[] = {install_warnings_call,    install_packages_call,
                                     install_plan_call,        install_detect_call,
                                     install_entry_call,       install_runtime_call,
                                     install_application_call, install_register_loader_call,
                                     install_by_id_call,       install_ecosystem_call};
    const runtime_install_definition *install = runtime_install_definition_of();
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    catalog_request held;
    catalog_answer result = {0, NULL};
    spisor_status status;
    memset(&held, 0, sizeof held);
    status = callers[operation](install, request, &held, &result.tree, &refusal);
    if (status != SPISOR_OK) {
        result.tree = cache_refusal_tree(&refusal);
    }
    install->release_refusal(install->context, &refusal);
    catalog_request_release(&held);
    cJSON_Delete(request);
    result.status = (int32_t)status;
    return result;
}

#endif
