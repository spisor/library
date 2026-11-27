#ifndef RUNTIME_TEST_HOSTS_CALL_H
#define RUNTIME_TEST_HOSTS_CALL_H

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "install_call.h"
#include "registry_call.h"
#include "runtime/hosts.h"
#include "spisor/status.h"

/**
 * @file
 * Calls the hosts definition with a request a test writes as JSON, in the shape hosts' operations take inside runtime,
 * and answers what the definition answered as that operation's JSON, so a test reads an answer the way runtime's own
 * callers do. A refusal answers {message, fix} with the definition's status. Test code only: the JSON is the test's
 * data, never the boundary.
 */

typedef enum hosts_operation {
    HOSTS_REGISTER,
    HOSTS_LIST,
    HOSTS_REMOVE,
    HOSTS_PROVENANCE,
    HOSTS_START
} hosts_operation;

/** What a request's JSON held, borrowed from it, released by hosts_request_release. */
typedef struct hosts_request {
    catalog_request held;
    registry_parts parts;
    runtime_install_origin origin;
    runtime_item_version layer;
} hosts_request;

static inline const runtime_hosts_command *hosts_commands_of(hosts_request *read, const cJSON *commands,
                                                             size_t *count) {
    runtime_hosts_command *filled = catalog_request_block(&read->held, (size_t)cJSON_GetArraySize(commands),
                                                          sizeof *filled);
    const cJSON *command;
    *count = 0;
    cJSON_ArrayForEach(command, commands) {
        runtime_hosts_command *each = &filled[(*count)++];
        const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(command, "arguments");
        const char **texts = catalog_request_block(&read->held, (size_t)cJSON_GetArraySize(arguments), sizeof *texts);
        const cJSON *argument;
        each->name = cache_member_text(command, "name");
        each->target = cache_member_text(command, "target");
        each->arguments = texts;
        cJSON_ArrayForEach(argument, arguments) {
            texts[each->argument_count++] = cJSON_GetStringValue(argument);
        }
    }
    return filled;
}

static inline spisor_status hosts_register_call(const runtime_hosts_definition *hosts, const cJSON *request,
                                                hosts_request *read, cJSON **tree, spisor_refusal *refusal) {
    const cJSON *identity = cJSON_GetObjectItemCaseSensitive(request, "identity");
    cJSON *record = cJSON_IsObject(identity) ? cJSON_Duplicate(identity, 1) : NULL;
    runtime_hosts_register_argument argument = SPISOR_INIT(runtime_hosts_register_argument);
    runtime_install_register_loader_answer answer = SPISOR_INIT(runtime_install_register_loader_answer);
    spisor_status status;
    if (record != NULL && cJSON_HasObjectItem(request, "provenance")) {
        CHECK(cJSON_AddItemToObject(record, "provenance",
                                    cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(request, "provenance"), 1)));
    }
    argument.state = install_place_of(request, "state");
    argument.registration = registry_parts_of(record, &read->parts);
    argument.commands = hosts_commands_of(read, cJSON_GetObjectItemCaseSensitive(identity, "commands"),
                                          &argument.command_count);
    argument.environment = install_environment_of(&read->held, request, &argument.environment_count);
    status = hosts->register_host(hosts->context, &argument, &answer, refusal);
    cJSON_Delete(record);
    if (status == SPISOR_OK) {
        *tree = cJSON_CreateObject();
        CHECK(cJSON_AddItemToObject(*tree, "provenance", install_provenance_json(answer.provenance)));
        CHECK(cJSON_AddItemToObject(*tree, "commands", cache_texts_json(answer.commands, answer.command_count)));
        CHECK(cJSON_AddItemToObject(
            *tree, "refusedCommands",
            install_refused_commands_json(answer.refused_commands, answer.refused_command_count)));
    }
    hosts->release_register_answer(hosts->context, &answer);
    return status;
}

static inline spisor_status hosts_list_call(const runtime_hosts_definition *hosts, const cJSON *request,
                                            hosts_request *read, cJSON **tree, spisor_refusal *refusal) {
    runtime_registry_list_argument argument = SPISOR_INIT(runtime_registry_list_argument);
    runtime_registry_listing answer = SPISOR_INIT(runtime_registry_listing);
    spisor_status status;
    (void)read;
    argument.state = install_place_of(request, "state");
    status = hosts->list(hosts->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        registry_result listed;
        memset(&listed, 0, sizeof listed);
        listed.tree = cJSON_CreateObject();
        registry_answer_listing(&listed, &answer);
        *tree = listed.tree;
    }
    hosts->release_listing(hosts->context, &answer);
    return status;
}

static inline cJSON *hosts_drifted_json(const runtime_change_drift *drifted, size_t count) {
    cJSON *array = cJSON_CreateArray();
    for (size_t index = 0; index < count; index++) {
        cJSON *each = cJSON_CreateObject();
        CHECK(cJSON_AddNumberToObject(each, "sequence", (double)drifted[index].sequence) != NULL);
        cache_add_text(each, "kind", drifted[index].kind);
        cache_add_text(each, "detail", drifted[index].detail);
        CHECK(cJSON_AddItemToArray(array, each));
    }
    return array;
}

static inline spisor_status hosts_remove_call(const runtime_hosts_definition *hosts, const cJSON *request,
                                              hosts_request *read, cJSON **tree, spisor_refusal *refusal) {
    runtime_registry_forget_argument argument = SPISOR_INIT(runtime_registry_forget_argument);
    runtime_hosts_remove_answer answer = SPISOR_INIT(runtime_hosts_remove_answer);
    spisor_status status;
    (void)read;
    argument.state = install_place_of(request, "state");
    argument.cache = install_place_of(request, "cache");
    argument.id = cache_member_text(request, "id");
    status = hosts->remove(hosts->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        *tree = cJSON_CreateObject();
        CHECK(cJSON_AddNumberToObject(*tree, "reversed", (double)answer.reversed) != NULL);
        CHECK(cJSON_AddItemToObject(*tree, "drifted", hosts_drifted_json(answer.drifted, answer.drifted_count)));
        CHECK(cJSON_AddBoolToObject(*tree, "forgotten", answer.forgotten != 0) != NULL);
    }
    hosts->release_remove_answer(hosts->context, &answer);
    return status;
}

static inline spisor_status hosts_provenance_call(const runtime_hosts_definition *hosts, const cJSON *request,
                                                  hosts_request *read, cJSON **tree, spisor_refusal *refusal) {
    const cJSON *origin = cJSON_GetObjectItemCaseSensitive(request, "origin");
    const cJSON *now = cJSON_GetObjectItemCaseSensitive(request, "now");
    runtime_hosts_provenance_argument argument = SPISOR_INIT(runtime_hosts_provenance_argument);
    runtime_hosts_provenance_answer answer = SPISOR_INIT(runtime_hosts_provenance_answer);
    spisor_status status;
    read->origin.package.method = cache_member_text(origin, "method");
    read->origin.package.name = cache_member_text(origin, "package");
    read->origin.package.options = catalog_request_value(&read->held, origin, "options");
    read->origin.version = cache_member_text(origin, "version");
    argument.origin = cJSON_IsObject(origin) ? &read->origin : NULL;
    argument.binary = cache_member_text(cJSON_GetObjectItemCaseSensitive(request, "application"), "binary");
    argument.home = cache_member_text(request, "home");
    argument.environment = install_environment_of(&read->held, request, &argument.environment_count);
    argument.now = now == NULL ? 0 : cJSON_IsNumber(now) ? now->valuedouble : -1;
    status = hosts->provenance(hosts->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        *tree = install_provenance_json(answer.provenance);
    }
    hosts->release_provenance_answer(hosts->context, &answer);
    return status;
}

static inline cJSON *hosts_versions_json(const runtime_item_version *versions, size_t count) {
    cJSON *object = cJSON_CreateObject();
    for (size_t index = 0; index < count; index++) {
        cache_add_text(object, versions[index].id, versions[index].version);
    }
    return object;
}

static inline cJSON *hosts_plan_json(const runtime_updates_apply_answer *plan, int reused) {
    cJSON *json = cJSON_CreateObject();
    cJSON *unrecorded = cJSON_AddArrayToObject(json, "unrecorded");
    CHECK(cJSON_AddItemToObject(json, "plugins", hosts_versions_json(plan->plugins, plan->plugin_count)));
    CHECK(cJSON_AddItemToObject(json, "run", hosts_versions_json(plan->run, plan->run_count)));
    if (!reused) {
        CHECK(cJSON_AddItemToObject(json, "next", hosts_versions_json(plan->next, plan->next_count)));
        CHECK(cJSON_AddItemToObject(json, "rollback", hosts_versions_json(plan->rollback, plan->rollback_count)));
    }
    for (size_t index = 0; index < plan->unrecorded_count; index++) {
        cJSON *each = cJSON_CreateObject();
        cache_add_text(each, "message", plan->unrecorded[index].detail);
        cache_add_text(each, "fix", plan->unrecorded[index].fix);
        CHECK(cJSON_AddItemToArray(unrecorded, each));
    }
    cache_add_text(json, "runRecord", plan->run_record);
    return json;
}

static inline cJSON *hosts_places_json(const runtime_places_answer *places) {
    cJSON *json = cJSON_CreateObject();
    cache_add_text(json, "home", places->home);
    cache_add_text(json, "portable", places->portable);
    cache_add_text(json, "state", places->state);
    cache_add_text(json, "cache", places->cache);
    cache_add_text(json, "secretsFile", places->secrets_file);
    CHECK(cJSON_AddBoolToObject(json, "forceFileStore", places->force_file_store != 0) != NULL);
    return json;
}

static inline cJSON *hosts_start_json(const runtime_hosts_start_answer *answer) {
    cJSON *tree = cJSON_CreateObject();
    cJSON *identity = cJSON_CreateObject();
    cache_add_text(identity, "version", answer->version);
    cache_add_text(identity, "home", answer->home);
    CHECK(cJSON_AddBoolToObject(identity, "requiresCommand", answer->requires_command != 0) != NULL);
    install_add_value(identity, "application", answer->application);
    CHECK(cJSON_AddItemToObject(tree, "places", hosts_places_json(answer->places)));
    CHECK(cJSON_AddItemToObject(tree, "identity", identity));
    install_add_value(tree, "settings", answer->settings);
    CHECK(cJSON_AddItemToObject(tree, "plan", hosts_plan_json(answer->plan, answer->reused_recorded_run != 0)));
    CHECK(cJSON_AddBoolToObject(tree, "reusedRecordedRun", answer->reused_recorded_run != 0) != NULL);
    if (answer->timers != NULL) {
        cJSON *timers = cJSON_AddObjectToObject(tree, "timers");
        CHECK(cJSON_AddNumberToObject(timers, "recordRunMilliseconds", answer->timers->record_run_milliseconds) !=
              NULL);
        CHECK(cJSON_AddNumberToObject(timers, "pruneFailuresBeforeExit",
                                      (double)answer->timers->prune_failures_before_exit) != NULL);
        CHECK(cJSON_AddNumberToObject(timers, "updateCheckMilliseconds", answer->timers->update_check_milliseconds) !=
              NULL);
    }
    CHECK(cJSON_AddBoolToObject(tree, "noColor", answer->no_color != 0) != NULL);
    return tree;
}

static inline const runtime_item_version *hosts_versions_of(hosts_request *read, const cJSON *object, size_t *count) {
    runtime_item_version *versions = catalog_request_block(&read->held, (size_t)cJSON_GetArraySize(object),
                                                           sizeof *versions);
    const cJSON *member;
    *count = 0;
    cJSON_ArrayForEach(member, object) {
        versions[*count].id = member->string;
        versions[(*count)++].version = cJSON_GetStringValue(member);
    }
    return versions;
}

static inline spisor_status hosts_start_call(const runtime_hosts_definition *hosts, const cJSON *request,
                                             hosts_request *read, cJSON **tree, spisor_refusal *refusal) {
    const cJSON *layer = cJSON_GetObjectItemCaseSensitive(request, "layer");
    runtime_hosts_start_argument argument = SPISOR_INIT(runtime_hosts_start_argument);
    runtime_hosts_start_answer answer = SPISOR_INIT(runtime_hosts_start_answer);
    spisor_status status;
    read->layer.id = cache_member_text(layer, "id");
    read->layer.version = cache_member_text(layer, "version");
    argument.environment = install_environment_of(&read->held, request, &argument.environment_count);
    argument.host_id = cache_member_text(request, "hostId");
    argument.layer = cJSON_IsObject(layer) ? &read->layer : NULL;
    argument.libraries = hosts_versions_of(read, cJSON_GetObjectItemCaseSensitive(request, "libraries"),
                                           &argument.library_count);
    argument.session = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "session")) ? 1 : 0;
    argument.layer_language = cache_member_text(request, "layerLanguage");
    argument.action_plugin = cache_member_text(request, "actionPlugin");
    status = hosts->start(hosts->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        *tree = hosts_start_json(&answer);
    }
    hosts->release_start_answer(hosts->context, &answer);
    return status;
}

typedef spisor_status (*hosts_caller)(const runtime_hosts_definition *hosts, const cJSON *request,
                                      hosts_request *read, cJSON **tree, spisor_refusal *refusal);

/** Calls `operation` with `request`, which it frees, answering the definition's status and the answer as JSON. */
static inline catalog_answer hosts_call(hosts_operation operation, cJSON *request) {
    static const hosts_caller callers[] = {hosts_register_call, hosts_list_call, hosts_remove_call,
                                           hosts_provenance_call, hosts_start_call};
    const runtime_hosts_definition *hosts = runtime_hosts_definition_of();
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    catalog_answer result = {0, NULL};
    hosts_request read;
    spisor_status status;
    memset(&read, 0, sizeof read);
    status = callers[operation](hosts, request, &read, &result.tree, &refusal);
    if (status != SPISOR_OK) {
        result.tree = cache_refusal_tree(&refusal);
    }
    hosts->release_refusal(hosts->context, &refusal);
    registry_release_parts(&read.parts);
    catalog_request_release(&read.held);
    cJSON_Delete(request);
    result.status = (int32_t)status;
    return result;
}

#endif
