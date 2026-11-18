#include "install_by_name.h"
#include "allocation.h"

#include <stdlib.h>
#include <string.h>

#include "cache_refusal.h"
#include "host_definition.h"
#include "host_output.h"
#include "host_service.h"
#include "install_by_id.h"
#include "install_warning_report.h"
#include "marketplace_copies.h"
#include "marketplace_lookup.h"
#include "marketplace_memory.h"
#include "marketplace_name_lookup.h"
#include "runtime/install.h"
#include "runtime/marketplace.h"
#include "runtime/paths.h"

#define NO_MARKETPLACE_FIX "list marketplaces in the portable settings.json as marketplaces: [{id, url}], or fix the ones refused above"
#define RUN_AGAIN_FIX "run the command again"

/** The name as it was found: which marketplace gave which id, and how the user asked for it. */
typedef struct found_name {
    const char *marketplace;
    const char *id;
    const char *bare;
    int was_asked_bare;
} found_name;

/** The part of a name after its marketplace, as marketplace.lookup splits it at the first slash. */
static const char *bare_name_of(const char *name) {
    const char *slash = strchr(name, '/');
    return slash == NULL ? name : slash + 1;
}

static const char *text_or_empty(const char *text) {
    return text == NULL ? "" : text;
}

static int refuse_out_of_memory(void) {
    return host_refuse_printably(NULL);
}

static int refuse_with(const char *const *parts, size_t count, const char *fix) {
    return host_refuse_printably(host_refusal_of(parts, count, fix));
}

static int refuse_with_fix_parts(const char *const *parts, size_t count, const char *const *fix_parts, size_t fix_count) {
    char *fix = runtime_cache_text(fix_parts, fix_count);
    int code = refuse_with(parts, count, fix == NULL ? RUN_AGAIN_FIX : fix);
    spisor_memory_release(fix);
    return code;
}

static int refuse_ambiguous(const char *name, const char *const *forms, size_t count) {
    char *joined_forms = NULL;
    for (size_t index = 0; index < count; index++) {
        const char *parts[3] = {joined_forms == NULL ? "" : joined_forms, joined_forms == NULL ? "" : ", ",
                                forms[index]};
        char *longer = runtime_cache_text(parts, 3);
        spisor_memory_release(joined_forms);
        joined_forms = longer;
    }
    const char *parts[3] = {name, " is in more than one marketplace under different ids: ", joined_forms == NULL ? "" : joined_forms};
    int code = refuse_with(parts, 3, "install it by the form that names the marketplace you mean, as --name <marketplace>/<name>");
    spisor_memory_release(joined_forms);
    return code;
}

static int refuse_not_found(const char *name) {
    const char *parts[2] = {name, " is not listed in any marketplace that could be read"};
    return refuse_with(parts, 2, "check the spelling, or name its marketplace as <marketplace>/<name>");
}

static int refuse_changed_name(const char *label, const char *was, const char *now, const char *application) {
    const char *parts[5] = {label, " was ", was, " when it was first installed, and it now means ", now};
    const char *fix_parts[8] = {"check that ", now, " is the plugin you mean, then run spisor-host install --name ", label,
                                " --application ", application, " --accept ", now};
    return refuse_with_fix_parts(parts, 5, fix_parts, 8);
}

static int refuse_changed_primary(const char *name, const char *application,
                                  const runtime_install_by_id_answer *answer) {
    const char *primary = answer->primary;
    const char *parts[5] = {name, " is recorded as installed from ", answer->recorded_primary,
                            ", and its repo's copies now name this as the primary: ", primary};
    const char *fix_parts[8] = {"check that ", primary, " is the plugin you mean, then run spisor-host install --name ", name,
                                " --application ", application, " --accept-primary ", primary};
    return refuse_with_fix_parts(parts, 5, fix_parts, 8);
}

static int refuse_no_marketplace(const char *name) {
    const char *parts[3] = {"no marketplace could be read, so ", name, " cannot be looked up"};
    return refuse_with(parts, 3, NO_MARKETPLACE_FIX);
}

/** The plugin folder under home, resolved by the paths service. NULL with *refusal set. */
static char *plugin_directory_under(const char *home, cJSON **refusal) {
    const runtime_paths_definition *paths = runtime_paths_definition_of();
    runtime_paths_resolve_argument argument = SPISOR_INIT(runtime_paths_resolve_argument);
    runtime_paths_answer answer = SPISOR_INIT(runtime_paths_answer);
    spisor_refusal refused = SPISOR_INIT(spisor_refusal);
    char *directory = NULL;
    argument.home = home;
    if (paths->resolve(paths->context, &argument, &answer, &refused) == SPISOR_OK) {
        directory = spisor_memory_allocate(strlen(answer.plugin) + 1);
        if (directory != NULL) {
            strcpy(directory, answer.plugin);
        }
    } else {
        *refusal = host_refusal_json(&refused);
    }
    paths->release_answer(paths->context, &answer);
    paths->release_refusal(paths->context, &refused);
    return directory;
}

/** The plugin folder of the registered application: its home, resolved by the paths service. NULL with *refusal set. */
static char *plugin_directory_of(const host_context *context, const char *application, cJSON **refusal) {
    cJSON *registration = host_registration(context, application, refusal);
    const char *home = host_text(registration, "home");
    char *directory = home == NULL ? NULL : plugin_directory_under(home, refusal);
    cJSON_Delete(registration);
    return directory;
}

/** What install.byId answered, as it is printed: unset texts as empty ones. NULL when memory runs out. */
static cJSON *installed_json(const runtime_install_by_id_answer *installed) {
    cJSON *json = cJSON_CreateObject();
    cJSON *ids = json == NULL ? NULL : cJSON_CreateArray();
    int built = ids != NULL;
    for (size_t index = 0; built && index < installed->id_count; index++) {
        cJSON *id = cJSON_CreateString(installed->ids[index]);
        built = id != NULL && cJSON_AddItemToArray(ids, id);
        if (!built) {
            cJSON_Delete(id);
        }
    }
    built = built && cJSON_AddStringToObject(json, "id", text_or_empty(installed->id)) != NULL &&
            cJSON_AddStringToObject(json, "name", text_or_empty(installed->name)) != NULL &&
            cJSON_AddStringToObject(json, "fullName", text_or_empty(installed->full_name)) != NULL &&
            cJSON_AddItemToObject(json, "ids", ids);
    if (!built) {
        cJSON_Delete(ids);
        cJSON_Delete(json);
        return NULL;
    }
    built = cJSON_AddStringToObject(json, "primary", text_or_empty(installed->primary)) != NULL &&
            cJSON_AddBoolToObject(json, "primaryConfirmed", installed->primary_confirmed != 0) != NULL &&
            cJSON_AddStringToObject(json, "version", text_or_empty(installed->version)) != NULL &&
            cJSON_AddStringToObject(json, "commit", text_or_empty(installed->commit)) != NULL &&
            cJSON_AddStringToObject(json, "sha256", text_or_empty(installed->sha256)) != NULL;
    if (!built) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

/** A report in the shape host_print_install_warnings reads, holding the one plugin install.byId installed. */
static cJSON *report_of(const runtime_install_by_id_answer *installed) {
    cJSON *report = cJSON_CreateObject();
    cJSON *rows = report == NULL ? NULL : cJSON_AddArrayToObject(report, "installed");
    cJSON *row = rows == NULL ? NULL : cJSON_CreateObject();
    if (row == NULL || cJSON_AddStringToObject(row, "id", text_or_empty(installed->id)) == NULL ||
        cJSON_AddStringToObject(row, "version", text_or_empty(installed->version)) == NULL ||
        !cJSON_AddItemToArray(rows, row)) {
        cJSON_Delete(row);
        cJSON_Delete(report);
        return NULL;
    }
    return report;
}

static int print_installed(const host_context *context, const runtime_install_by_id_answer *installed) {
    cJSON *printed = installed_json(installed);
    if (printed == NULL) {
        return refuse_out_of_memory();
    }
    host_print_json(printed);
    cJSON_Delete(printed);
    cJSON *report = report_of(installed);
    host_print_install_warnings(context, report);
    cJSON_Delete(report);
    return HOST_EXIT_DONE;
}

/**
 * Asks marketplace.remember about the name under marketplace, or under its bare name when marketplace is NULL, recording
 * it only when is_recording. 1 when it is settled; 0 with *code the exit code after saying why it is not.
 */
static int settles_name(const host_context *context, const host_name_request *request, const found_name *found,
                        const char *marketplace, int is_recording, int *code) {
    const runtime_marketplace_definition *names = runtime_marketplace_definition_of();
    runtime_marketplace_remember_argument argument = SPISOR_INIT(runtime_marketplace_remember_argument);
    runtime_marketplace_remember_answer answer = SPISOR_INIT(runtime_marketplace_remember_answer);
    spisor_refusal declined = SPISOR_INIT(spisor_refusal);
    argument.state = context->state;
    argument.marketplace = marketplace;
    argument.name = found->bare;
    argument.id = found->id;
    argument.accept = request->accepted_name_id;
    argument.check_only = is_recording ? 0 : 1;
    spisor_status status = names->remember(names->context, &argument, &answer, &declined);
    int is_settled = status == SPISOR_OK && answer.memory != RUNTIME_MARKETPLACE_NEEDS_USER_ACCEPTANCE;
    if (status != SPISOR_OK) {
        *code = host_refuse_printably(host_refusal_json(&declined));
    } else if (!is_settled) {
        const char *label_parts[3] = {marketplace == NULL ? "" : marketplace, "/", found->bare};
        char *label = marketplace == NULL ? NULL : runtime_cache_text(label_parts, 3);
        const char *shown = marketplace == NULL || label == NULL ? found->bare : label;
        *code = refuse_changed_name(shown, answer.was, answer.now, request->application);
        spisor_memory_release(label);
    }
    names->release_remember_answer(names->context, &answer);
    names->release_refusal(names->context, &declined);
    return is_settled;
}

/** Settles the qualified name and, when the user asked for a bare name, the bare one too. */
static int settles_names(const host_context *context, const host_name_request *request, const found_name *found, int is_recording,
                         int *code) {
    return settles_name(context, request, found, found->marketplace, is_recording, code) &&
           (!found->was_asked_bare || settles_name(context, request, found, NULL, is_recording, code));
}

static int install_found(const host_context *context, const host_name_request *request, const found_name *found) {
    cJSON *refusal = NULL;
    char *plugin_directory = plugin_directory_of(context, request->application, &refusal);
    if (plugin_directory == NULL) {
        return refusal == NULL ? refuse_out_of_memory() : host_refuse_printably(refusal);
    }
    int code = HOST_EXIT_DONE;
    if (!settles_names(context, request, found, 0, &code)) {
        spisor_memory_release(plugin_directory);
        return code;
    }
    const runtime_install_definition *install = runtime_install_definition_of();
    runtime_install_by_id_argument argument = SPISOR_INIT(runtime_install_by_id_argument);
    runtime_install_by_id_answer installed = SPISOR_INIT(runtime_install_by_id_answer);
    spisor_refusal declined = SPISOR_INIT(spisor_refusal);
    argument.id = found->id;
    argument.cache = context->cache;
    argument.plugin_directory = plugin_directory;
    argument.accept_primary = request->accepted_primary;
    argument.api_base = host_marketplace_lookup_api_base();
    spisor_status status = install->by_id(install->context, &argument, &installed, &declined);
    spisor_memory_release(plugin_directory);
    if (status != SPISOR_OK) {
        code = host_refuse_printably(host_refusal_json(&declined));
    } else if (installed.needs_user_acceptance) {
        code = refuse_changed_primary(request->name, request->application, &installed);
    } else if (settles_names(context, request, found, 1, &code)) {
        code = print_installed(context, &installed);
    }
    install->release_by_id_answer(install->context, &installed);
    install->release_refusal(install->context, &declined);
    return code;
}

static int install_looked_up(const host_context *context, const host_name_request *request,
                             const runtime_marketplace_lookup_answer *answer) {
    if (answer->match == RUNTIME_MARKETPLACE_AMBIGUOUS) {
        return refuse_ambiguous(request->name, answer->ambiguous, answer->ambiguous_count);
    }
    found_name found = {answer->marketplace, answer->id, bare_name_of(request->name),
                        strchr(request->name, '/') == NULL};
    return found.id == NULL || found.marketplace == NULL ? refuse_not_found(request->name)
                                                         : install_found(context, request, &found);
}

/** Looks the name up in the copies and installs what it means. */
static int look_up_and_install(const host_context *context, const host_name_request *request,
                               const host_marketplace_copies *copies) {
    const runtime_marketplace_definition *marketplace = runtime_marketplace_definition_of();
    runtime_marketplace_lookup_argument argument = SPISOR_INIT(runtime_marketplace_lookup_argument);
    runtime_marketplace_lookup_answer answer = SPISOR_INIT(runtime_marketplace_lookup_answer);
    spisor_refusal declined = SPISOR_INIT(spisor_refusal);
    argument.copies = copies->copies;
    argument.copy_count = copies->count;
    argument.name = request->name;
    spisor_status status = marketplace->lookup(marketplace->context, &argument, &answer, &declined);
    int code = status == SPISOR_OK     ? install_looked_up(context, request, &answer)
               : status == SPISOR_NONE ? refuse_not_found(request->name)
                                       : host_refuse_printably(host_refusal_json(&declined));
    marketplace->release_lookup_answer(marketplace->context, &answer);
    marketplace->release_refusal(marketplace->context, &declined);
    return code;
}

int host_install_by_name(const host_context *context, const host_name_request *request) {
    cJSON *refusal = NULL;
    host_marketplace_copies copies;
    int code;
    if (!host_read_marketplace_copies(context, &copies, &refusal)) {
        code = refusal == NULL ? refuse_out_of_memory() : host_refuse_printably(refusal);
    } else if (copies.count == 0) {
        code = refuse_no_marketplace(request->name);
    } else {
        code = look_up_and_install(context, request, &copies);
    }
    host_release_marketplace_copies(&copies);
    return code;
}
