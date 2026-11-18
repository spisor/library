#include "loader_start.h"
#include "allocation.h"

#include <stdlib.h>
#include <string.h>

#include "host_definition.h"
#include "host_output.h"
#include "json_value.h"
#include "runtime/install.h"
#include "runtime/loader.h"
#include "runtime/processes.h"
#include "runtime/updates.h"

static void remove_run_record(const host_context *context, const char *id, const char *record) {
    const runtime_processes_definition *processes = runtime_processes_definition_of();
    runtime_processes_remove_argument argument = SPISOR_INIT(runtime_processes_remove_argument);
    argument.state = context->state;
    argument.host_id = id;
    argument.record = record;
    processes->remove_runs(processes->context, &argument, NULL);
}

static const char *version_of(const runtime_item_version *versions, size_t count, const char *id) {
    for (size_t index = 0; index < count; index++) {
        if (strcmp(versions[index].id, id) == 0) {
            return versions[index].version;
        }
    }
    return NULL;
}

/** {id: version} of each plugin; NULL when memory runs out. */
static cJSON *plugins_json(const runtime_item_version *plugins, size_t count) {
    cJSON *json = cJSON_CreateObject();
    for (size_t index = 0; json != NULL && index < count; index++) {
        if (cJSON_AddStringToObject(json, plugins[index].id, plugins[index].version) == NULL) {
            cJSON_Delete(json);
            json = NULL;
        }
    }
    return json;
}

/** Keeps the plugins the start runs and the version of id it runs; 0 when memory runs out. */
static int keep_applied(loader_start *start, const char *id, const runtime_updates_apply_answer *applied) {
    const char *version = version_of(applied->run, applied->run_count, id);
    start->plugins = plugins_json(applied->plugins, applied->plugin_count);
    start->version = version == NULL ? NULL : spisor_memory_allocate(strlen(version) + 1);
    if (start->version != NULL) {
        strcpy(start->version, version);
    }
    return start->plugins != NULL && (version == NULL || start->version != NULL);
}

/**
 * updates.apply for id: the plugins its settings enable, and the version it runs into start.
 *
 * @implNote running names the registered version, which a start runs when its references record none, as
 * after they were removed; a scheduled version installed intact still wins.
 */
static int apply(const host_context *context, const char *id, loader_start *start, cJSON **refusal) {
    const runtime_updates_definition *updates = runtime_updates_definition_of();
    runtime_updates_apply_argument argument = SPISOR_INIT(runtime_updates_apply_argument);
    runtime_updates_apply_answer applied = SPISOR_INIT(runtime_updates_apply_answer);
    spisor_refusal declined = SPISOR_INIT(spisor_refusal);
    spisor_value_tree tree = SPISOR_INIT(spisor_value_tree);
    runtime_item_version running = {id, host_text(start->registration, "version")};
    cJSON *settings = host_settings_of(context, host_text(start->registration, "home"), refusal);
    spisor_status status = settings == NULL ? SPISOR_OUT_OF_MEMORY : spisor_value_of_json(settings, &tree);
    argument.state = context->state;
    argument.cache = context->cache;
    argument.host_id = id;
    argument.settings = &tree.root;
    argument.running = &running;
    argument.running_count = running.version == NULL ? 0 : 1;
    if (status == SPISOR_OK) {
        status = updates->apply(updates->context, &argument, &applied, &declined);
        *refusal = status == SPISOR_OK ? NULL : host_refusal_json(&declined);
    }
    if (status == SPISOR_OK && applied.run_record != NULL) {
        remove_run_record(context, id, applied.run_record);
    }
    int kept = status == SPISOR_OK && keep_applied(start, id, &applied);
    updates->release_apply_answer(updates->context, &applied);
    updates->release_refusal(updates->context, &declined);
    spisor_value_release(&tree);
    cJSON_Delete(settings);
    return kept;
}

/**
 * Registers loader id at version again, requesting its commands, and writes what it could not do to stderr.
 *
 * @implNote Never refuses the start (RULES section 2): the registration and its commands stand from the
 * last time they could be written, and a shim rewrite can fail, a running image above all.
 */
static void register_again(const host_context *context, const char *id, const char *version) {
    const runtime_install_definition *install = runtime_install_definition_of();
    runtime_install_register_loader_argument argument = SPISOR_INIT(runtime_install_register_loader_argument);
    runtime_install_register_loader_answer registered = SPISOR_INIT(runtime_install_register_loader_answer);
    spisor_refusal declined = SPISOR_INIT(spisor_refusal);
    runtime_variable *environment = host_variables_of(context->environment, &argument.environment_count);
    argument.portable = context->portable;
    argument.state = context->state;
    argument.cache = context->cache;
    argument.environment = environment;
    argument.id = id;
    argument.version = version;
    spisor_status status = environment == NULL ? SPISOR_OUT_OF_MEMORY
                                               : install->register_loader(install->context, &argument, &registered,
                                                                          &declined);
    if (status != SPISOR_OK) {
        cJSON *refusal = host_refusal_json(&declined);
        host_write(1, "spisor could not register ");
        host_write(1, id);
        host_write(1, " again: ");
        host_print_refusal(refusal);
        cJSON_Delete(refusal);
    }
    install->release_register_loader_answer(install->context, &registered);
    install->release_refusal(install->context, &declined);
    spisor_memory_release(environment);
}

/** The loader standard id's installed version carries, as JSON; NULL with *refusal set. */
static cJSON *installed_standard(const host_context *context, const char *id, const char *version, cJSON **refusal) {
    const runtime_loader_definition *loader = runtime_loader_definition_of();
    runtime_item_version installed_loader = {id, version};
    runtime_loader_installed_argument argument = SPISOR_INIT(runtime_loader_installed_argument);
    runtime_loader_installed_answer installed = SPISOR_INIT(runtime_loader_installed_answer);
    spisor_refusal declined = SPISOR_INIT(spisor_refusal);
    cJSON *standard = NULL;
    argument.cache = context->cache;
    argument.loader = &installed_loader;
    spisor_status status = loader->installed(loader->context, &argument, &installed, &declined);
    if (status == SPISOR_OK) {
        status = spisor_json_of_value(installed.standard, &standard);
    }
    *refusal = status == SPISOR_OK ? NULL : host_refusal_json(&declined);
    loader->release_installed_answer(loader->context, &installed);
    loader->release_refusal(loader->context, &declined);
    return standard;
}

int host_start_loader(const host_context *context, const char *id, loader_start *start, cJSON **refusal) {
    memset(start, 0, sizeof *start);
    start->registration = host_registration(context, id, refusal);
    if (start->registration == NULL || !apply(context, id, start, refusal) || start->version == NULL) {
        return 0;
    }
    register_again(context, id, start->version);
    start->standard = installed_standard(context, id, start->version, refusal);
    return start->standard != NULL;
}

void host_finish_loader(loader_start *start) {
    spisor_memory_release(start->version);
    cJSON_Delete(start->standard);
    cJSON_Delete(start->plugins);
    cJSON_Delete(start->registration);
    memset(start, 0, sizeof *start);
}
