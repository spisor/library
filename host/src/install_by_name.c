#include "install_by_name.h"

#include <stdlib.h>
#include <string.h>

#include "cache_refusal.h"
#include "host_output.h"
#include "host_service.h"
#include "install_by_id.h"
#include "install_warning_report.h"
#include "marketplace_copies.h"
#include "marketplace_lookup.h"
#include "marketplace_memory.h"
#include "marketplace_name_lookup.h"
#include "paths.h"

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

static cJSON *lookup_call(cJSON *copies, const char *name) {
    cJSON *argument = cJSON_CreateObject();
    if (argument == NULL || !cJSON_AddItemToObject(argument, "copies", copies) ||
        cJSON_AddStringToObject(argument, "name", name) == NULL) {
        cJSON_Delete(argument == NULL ? copies : NULL);
        cJSON_Delete(argument);
        return NULL;
    }
    return argument;
}

static cJSON *remember_call(const host_context *context, const found_name *found, const char *marketplace, const char *accepted_id,
                            int is_recording) {
    cJSON *argument = cJSON_CreateObject();
    int built = argument != NULL && cJSON_AddStringToObject(argument, "state", context->state) != NULL &&
                (marketplace == NULL || cJSON_AddStringToObject(argument, "marketplace", marketplace) != NULL) &&
                cJSON_AddStringToObject(argument, "name", found->bare) != NULL &&
                cJSON_AddStringToObject(argument, "id", found->id) != NULL &&
                (accepted_id == NULL || cJSON_AddStringToObject(argument, "accept", accepted_id) != NULL) &&
                (is_recording || cJSON_AddFalseToObject(argument, "record") != NULL);
    if (!built) {
        cJSON_Delete(argument);
        return NULL;
    }
    return argument;
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
    free(fix);
    return code;
}

static int refuse_ambiguous(const char *name, const cJSON *forms) {
    char *joined_forms = NULL;
    const cJSON *form;
    cJSON_ArrayForEach(form, forms) {
        const char *parts[3] = {joined_forms == NULL ? "" : joined_forms, joined_forms == NULL ? "" : ", ", form->valuestring};
        char *longer = runtime_cache_text(parts, 3);
        free(joined_forms);
        joined_forms = longer;
    }
    const char *parts[3] = {name, " is in more than one marketplace under different ids: ", joined_forms == NULL ? "" : joined_forms};
    int code = refuse_with(parts, 3, "install it by the form that names the marketplace you mean, as --name <marketplace>/<name>");
    free(joined_forms);
    return code;
}

static int refuse_not_found(const char *name) {
    const char *parts[2] = {name, " is not listed in any marketplace that could be read"};
    return refuse_with(parts, 2, "check the spelling, or name its marketplace as <marketplace>/<name>");
}

static int refuse_changed_name(const char *label, const char *was, const char *now, const char *application) {
    const char *parts[5] = {label, " was ", was, " when it was first installed, and it now means ", now};
    const char *fix_parts[8] = {"check that ", now, " is the plugin you mean, then run forebay-host install --name ", label,
                                " --application ", application, " --accept ", now};
    return refuse_with_fix_parts(parts, 5, fix_parts, 8);
}

static int refuse_changed_primary(const char *name, const char *application, const cJSON *answer) {
    const char *primary = host_text(answer, "primary");
    const char *parts[5] = {name, " is recorded as installed from ", host_text(answer, "recordedPrimary"),
                            ", and its repo's copies now name this as the primary: ", primary};
    const char *fix_parts[8] = {"check that ", primary, " is the plugin you mean, then run forebay-host install --name ", name,
                                " --application ", application, " --accept-primary ", primary};
    return refuse_with_fix_parts(parts, 5, fix_parts, 8);
}

static int refuse_no_marketplace(const char *name) {
    const char *parts[3] = {"no marketplace could be read, so ", name, " cannot be looked up"};
    return refuse_with(parts, 3, NO_MARKETPLACE_FIX);
}

/** The plugin folder of the registered application: its home, resolved by the paths service. NULL with *refusal set. */
static char *plugin_directory_of(const host_context *context, const char *application, cJSON **refusal) {
    cJSON *registration = host_registration(context, application, refusal);
    const char *home = host_text(registration, "home");
    char *directory = NULL;
    if (registration != NULL && home == NULL) {
        const char *parts[3] = {"the application ", application, " is registered without a home to deploy a plugin into"};
        *refusal = host_refusal_of(parts, 3, "start the application once so that it registers its home, then run the command again");
    }
    cJSON *argument = home == NULL ? NULL : cJSON_CreateObject();
    if (argument != NULL && cJSON_AddStringToObject(argument, "home", home) != NULL) {
        cJSON *paths = host_call(runtime_paths_resolve, argument, refusal);
        const char *plugin = host_text(paths, "plugin");
        if (plugin != NULL) {
            directory = malloc(strlen(plugin) + 1);
            if (directory != NULL) {
                strcpy(directory, plugin);
            }
        }
        cJSON_Delete(paths);
    } else {
        cJSON_Delete(argument);
    }
    cJSON_Delete(registration);
    return directory;
}

static cJSON *install_call(const host_context *context, const char *id, const char *plugin_directory, const char *accepted_primary) {
    cJSON *argument = cJSON_CreateObject();
    int built = argument != NULL && cJSON_AddStringToObject(argument, "id", id) != NULL &&
                cJSON_AddStringToObject(argument, "cache", context->cache) != NULL &&
                cJSON_AddStringToObject(argument, "pluginDir", plugin_directory) != NULL &&
                (accepted_primary == NULL || cJSON_AddStringToObject(argument, "acceptPrimary", accepted_primary) != NULL) &&
                (host_marketplace_lookup_api_base() == NULL ||
                 cJSON_AddStringToObject(argument, "apiBase", host_marketplace_lookup_api_base()) != NULL);
    if (!built) {
        cJSON_Delete(argument);
        return NULL;
    }
    return argument;
}

/** A report in the shape host_print_install_warnings reads, holding the one plugin install.byId installed. */
static cJSON *report_of(const cJSON *installed) {
    cJSON *report = cJSON_CreateObject();
    cJSON *rows = report == NULL ? NULL : cJSON_AddArrayToObject(report, "installed");
    cJSON *row = rows == NULL ? NULL : cJSON_CreateObject();
    if (row == NULL || cJSON_AddStringToObject(row, "id", host_text(installed, "id")) == NULL ||
        cJSON_AddStringToObject(row, "version", host_text(installed, "version")) == NULL || !cJSON_AddItemToArray(rows, row)) {
        cJSON_Delete(row);
        cJSON_Delete(report);
        return NULL;
    }
    return report;
}

static int print_installed(const host_context *context, const cJSON *installed) {
    host_print_json(installed);
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
    cJSON *refusal = NULL;
    cJSON *answer = host_call(runtime_marketplace_remember, remember_call(context, found, marketplace, request->accepted_name_id, is_recording),
                              &refusal);
    if (answer == NULL) {
        *code = refusal == NULL ? refuse_out_of_memory() : host_refuse_printably(refusal);
        return 0;
    }
    int is_settled = !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answer, "needsUserAcceptance"));
    if (!is_settled) {
        const char *label_parts[3] = {marketplace == NULL ? "" : marketplace, "/", found->bare};
        char *label = marketplace == NULL ? NULL : runtime_cache_text(label_parts, 3);
        *code = refuse_changed_name(marketplace == NULL ? found->bare : (label == NULL ? found->bare : label), host_text(answer, "was"),
                                    host_text(answer, "now"), request->application);
        free(label);
    }
    cJSON_Delete(answer);
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
        free(plugin_directory);
        return code;
    }
    cJSON *installed = host_call(runtime_install_by_id, install_call(context, found->id, plugin_directory, request->accepted_primary), &refusal);
    free(plugin_directory);
    if (installed == NULL) {
        return refusal == NULL ? refuse_out_of_memory() : host_refuse_printably(refusal);
    }
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(installed, "needsUserAcceptance"))) {
        code = refuse_changed_primary(request->name, request->application, installed);
    } else if (settles_names(context, request, found, 1, &code)) {
        code = print_installed(context, installed);
    }
    cJSON_Delete(installed);
    return code;
}

static int install_looked_up(const host_context *context, const host_name_request *request, const cJSON *answer) {
    const cJSON *forms = cJSON_GetObjectItemCaseSensitive(answer, "ambiguous");
    if (cJSON_IsArray(forms)) {
        return refuse_ambiguous(request->name, forms);
    }
    found_name found = {host_text(answer, "marketplace"), host_text(answer, "id"), bare_name_of(request->name),
                        strchr(request->name, '/') == NULL};
    return found.id == NULL || found.marketplace == NULL ? refuse_not_found(request->name) : install_found(context, request, &found);
}

int host_install_by_name(const host_context *context, const host_name_request *request) {
    cJSON *refusal = NULL;
    cJSON *copies = host_read_marketplace_copies(context, &refusal);
    if (copies == NULL) {
        return refusal == NULL ? refuse_out_of_memory() : host_refuse_printably(refusal);
    }
    if (cJSON_GetArraySize(copies) == 0) {
        cJSON_Delete(copies);
        return refuse_no_marketplace(request->name);
    }
    cJSON *answer = host_call(runtime_marketplace_lookup, lookup_call(copies, request->name), &refusal);
    if (answer == NULL) {
        return refusal == NULL ? refuse_out_of_memory() : host_refuse_printably(refusal);
    }
    int code = install_looked_up(context, request, answer);
    cJSON_Delete(answer);
    return code;
}
