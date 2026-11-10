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
#include "spisor/path.h"

#define NO_MARKETPLACE_FIX "list marketplaces in the portable settings.json as marketplaces: [{id, url}], or fix the ones refused above"

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

static cJSON *remember_call(const host_context *context, const char *marketplace, const char *name, const char *id,
                            const char *accepted_id) {
    cJSON *argument = cJSON_CreateObject();
    int built = argument != NULL && cJSON_AddStringToObject(argument, "state", context->state) != NULL &&
                cJSON_AddStringToObject(argument, "marketplace", marketplace) != NULL &&
                cJSON_AddStringToObject(argument, "name", name) != NULL && cJSON_AddStringToObject(argument, "id", id) != NULL &&
                (accepted_id == NULL || cJSON_AddStringToObject(argument, "accept", accepted_id) != NULL);
    if (!built) {
        cJSON_Delete(argument);
        return NULL;
    }
    return argument;
}

static char *plugin_directory(const host_context *context) {
    const char *segments[2] = {context->portable, "plugins"};
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
}

static cJSON *install_call(const host_context *context, const char *id, const char *accepted_id) {
    char *plugins = plugin_directory(context);
    cJSON *argument = plugins == NULL ? NULL : cJSON_CreateObject();
    int built = argument != NULL && cJSON_AddStringToObject(argument, "id", id) != NULL &&
                cJSON_AddStringToObject(argument, "cache", context->cache) != NULL &&
                cJSON_AddStringToObject(argument, "pluginDir", plugins) != NULL &&
                (accepted_id == NULL || cJSON_AddStringToObject(argument, "acceptPrimary", accepted_id) != NULL) &&
                (host_marketplace_lookup_api_base() == NULL ||
                 cJSON_AddStringToObject(argument, "apiBase", host_marketplace_lookup_api_base()) != NULL);
    free(plugins);
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
    int code = refuse_with(parts, count, fix == NULL ? "run the command again" : fix);
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

static int refuse_changed_name(const char *qualified, const char *was, const char *now) {
    const char *parts[5] = {qualified, " was ", was, " when it was first installed, and its marketplace now lists it as ", now};
    const char *fix_parts[6] = {"check that ", now, " is the plugin you mean, then run forebay-host install --name ", qualified, " --accept ", now};
    return refuse_with_fix_parts(parts, 5, fix_parts, 6);
}

static int refuse_changed_primary(const char *name, const cJSON *answer) {
    const char *primary = host_text(answer, "primary");
    const char *parts[5] = {name, " is recorded as installed from ", host_text(answer, "recordedPrimary"),
                            ", and its repo's copies now name this as the primary: ", primary};
    const char *fix_parts[6] = {"check that ", primary, " is the plugin you mean, then run forebay-host install --name ", name, " --accept ", primary};
    return refuse_with_fix_parts(parts, 5, fix_parts, 6);
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

static int install_found(const host_context *context, const char *name, const char *id, const char *accepted_id) {
    cJSON *refusal = NULL;
    cJSON *installed = host_call(runtime_install_by_id, install_call(context, id, accepted_id), &refusal);
    if (installed == NULL) {
        return host_refuse_printably(refusal);
    }
    int code = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(installed, "needsUserAcceptance"))
                   ? refuse_changed_primary(name, installed)
                   : print_installed(context, installed);
    cJSON_Delete(installed);
    return code;
}

/** Remembers id for the marketplace's name, then installs it; a name remembered as another id installs nothing. */
static int install_remembering(const host_context *context, const char *name, const cJSON *found, const char *accepted_id) {
    const char *marketplace = host_text(found, "marketplace");
    const char *id = host_text(found, "id");
    const char *bare = bare_name_of(name);
    cJSON *refusal = NULL;
    cJSON *remembered = host_call(runtime_marketplace_remember, remember_call(context, marketplace, bare, id, accepted_id), &refusal);
    if (remembered == NULL) {
        return refusal == NULL ? refuse_out_of_memory() : host_refuse_printably(refusal);
    }
    int code = HOST_EXIT_DONE;
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(remembered, "needsUserAcceptance"))) {
        const char *qualified_parts[3] = {marketplace, "/", bare};
        char *qualified = runtime_cache_text(qualified_parts, 3);
        code = qualified == NULL ? refuse_out_of_memory() : refuse_changed_name(qualified, host_text(remembered, "was"), host_text(remembered, "now"));
        free(qualified);
    } else {
        code = install_found(context, name, id, accepted_id);
    }
    cJSON_Delete(remembered);
    return code;
}

static int install_looked_up(const host_context *context, const char *name, const cJSON *answer, const char *accepted_id) {
    const cJSON *forms = cJSON_GetObjectItemCaseSensitive(answer, "ambiguous");
    if (cJSON_IsArray(forms)) {
        return refuse_ambiguous(name, forms);
    }
    return host_text(answer, "id") == NULL ? refuse_not_found(name) : install_remembering(context, name, answer, accepted_id);
}

int host_install_by_name(const host_context *context, const char *name, const char *accepted_id) {
    cJSON *refusal = NULL;
    cJSON *copies = host_read_marketplace_copies(context, &refusal);
    if (copies == NULL) {
        return refusal == NULL ? refuse_out_of_memory() : host_refuse_printably(refusal);
    }
    if (cJSON_GetArraySize(copies) == 0) {
        cJSON_Delete(copies);
        const char *parts[3] = {"no marketplace could be read, so ", name, " cannot be looked up"};
        return refuse_with(parts, 3, NO_MARKETPLACE_FIX);
    }
    cJSON *answer = host_call(runtime_marketplace_lookup, lookup_call(copies, name), &refusal);
    if (answer == NULL) {
        return refusal == NULL ? refuse_out_of_memory() : host_refuse_printably(refusal);
    }
    int code = install_looked_up(context, name, answer, accepted_id);
    cJSON_Delete(answer);
    return code;
}
