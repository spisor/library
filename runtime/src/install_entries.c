#include "install_entries.h"

#include <stdlib.h>
#include <string.h>

#include "boot_stub.h"
#include "cache_refusal.h"
#include "catalog_text.h"
#include "changes.h"
#include "install_support.h"
#include "json_service.h"
#include "paths.h"
#include "spisor/json_write.h"
#include "spisor/path.h"

#define MARKETPLACE_NAME "spisor"
#define PLUGIN_NAME "spisor"
#define ENABLED_PLUGIN PLUGIN_NAME "@" MARKETPLACE_NAME
#define PLUGIN_DESCRIPTION "Registers the spisor host each time a session starts"
#define HOOK_TIMEOUT_SECONDS 30
#define REINSTALL_FIX "run forebay-host install again"
#define ENTRY_FIX                                                                                            \
    "send {\"places\": {\"state\": \"<absolute directory>\", \"cache\": \"<absolute directory>\"}, \"loader\": " \
    "{\"id\", \"application\": {\"id\", \"label\"}, \"home\": \"<absolute directory>\"}}"

typedef struct entry_loader {
    const cJSON *places;
    const char *state;
    const char *cache;
    const char *id;
    const char *application;
    const char *label;
    const char *home;
} entry_loader;

typedef int (*entry_writer)(const entry_loader *loader, cJSON *changes);

static char *joined(const char *const *segments, size_t count) {
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, count);
}

static int add_change(cJSON *changes, cJSON *change) {
    if (change == NULL || !cJSON_AddItemToArray(changes, change)) {
        cJSON_Delete(change);
        return 0;
    }
    return 1;
}

static int add_directory(cJSON *changes, char *path) {
    cJSON *change = path == NULL ? NULL : cJSON_CreateObject();
    int built = change != NULL && cJSON_AddStringToObject(change, "kind", "directory") != NULL &&
                cJSON_AddStringToObject(change, "path", path) != NULL;
    free(path);
    if (!built) {
        cJSON_Delete(change);
        return 0;
    }
    return add_change(changes, change);
}

static int add_file(cJSON *changes, char *path, char *content) {
    cJSON *change = path == NULL || content == NULL ? NULL : cJSON_CreateObject();
    int built = change != NULL && cJSON_AddStringToObject(change, "kind", "file") != NULL &&
                cJSON_AddStringToObject(change, "path", path) != NULL &&
                cJSON_AddStringToObject(change, "content", content) != NULL;
    free(path);
    free(content);
    if (!built) {
        cJSON_Delete(change);
        return 0;
    }
    return add_change(changes, change);
}

/** JSON.stringify(value, null, 2) and a newline, the form the applications write their own JSON in; value is freed. */
static char *json_content(cJSON *value) {
    char *pretty = value == NULL ? NULL : spisor_json_stringify_pretty(value);
    cJSON_Delete(value);
    const char *parts[2] = {pretty, "\n"};
    char *content = pretty == NULL ? NULL : runtime_cache_text(parts, 2);
    free(pretty);
    return content;
}

static cJSON *parsed(const char *json) {
    return cJSON_Parse(json);
}

static cJSON *hook_arguments(const entry_loader *loader) {
    cJSON *arguments = cJSON_CreateArray();
    if (arguments == NULL || !runtime_install_add_text(arguments, RUNTIME_BOOTED_HOST_ID) ||
        !runtime_install_add_text(arguments, "hook") || !runtime_install_add_text(arguments, loader->id) ||
        !runtime_install_add_text(arguments, "session-start")) {
        cJSON_Delete(arguments);
        return NULL;
    }
    return arguments;
}

/**
 * @implNote Exec form (args present) spawns forebay-boot directly with no shell, so no path is ever
 * quoted for Git Bash or PowerShell, whichever Claude Code would pick on Windows.
 */
static cJSON *hooks_of(const entry_loader *loader) {
    char *boot = runtime_boot_stub_path(loader->cache);
    cJSON *hooks = boot == NULL ? NULL : parsed("{\"hooks\": {\"SessionStart\": [{\"hooks\": [{\"type\": \"command\"}]}]}}");
    cJSON *command = cJSON_GetArrayItem(
        cJSON_GetObjectItemCaseSensitive(
            cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(hooks, "hooks"), "SessionStart"), 0),
            "hooks"),
        0);
    cJSON *arguments = command == NULL ? NULL : hook_arguments(loader);
    int built = arguments != NULL && cJSON_AddStringToObject(command, "command", boot) != NULL &&
                cJSON_AddItemToObject(command, "args", arguments) &&
                cJSON_AddNumberToObject(command, "timeout", HOOK_TIMEOUT_SECONDS) != NULL;
    free(boot);
    if (!built) {
        if (cJSON_GetObjectItemCaseSensitive(command, "args") == NULL) {
            cJSON_Delete(arguments);
        }
        cJSON_Delete(hooks);
        return NULL;
    }
    return hooks;
}

static cJSON *settings_key(const entry_loader *loader, const char *member, const char *name, cJSON *value) {
    const char *segments[2] = {loader->home, "settings.json"};
    char *file = joined(segments, 2);
    cJSON *change = file == NULL || value == NULL ? NULL : cJSON_CreateObject();
    cJSON *key = change == NULL ? NULL : cJSON_AddArrayToObject(change, "key");
    int built = key != NULL && cJSON_AddStringToObject(change, "kind", "json-key") != NULL &&
                cJSON_AddStringToObject(change, "file", file) != NULL && runtime_install_add_text(key, member) &&
                runtime_install_add_text(key, name) && cJSON_AddItemToObject(change, "value", value) &&
                cJSON_AddBoolToObject(change, "create", 1) != NULL;
    free(file);
    if (!built) {
        if (change == NULL || cJSON_GetObjectItemCaseSensitive(change, "value") == NULL) {
            cJSON_Delete(value);
        }
        cJSON_Delete(change);
        return NULL;
    }
    return change;
}

static cJSON *marketplace_source(const char *root) {
    cJSON *source = parsed("{\"source\": {\"source\": \"directory\"}}");
    if (source != NULL && cJSON_AddStringToObject(cJSON_GetObjectItemCaseSensitive(source, "source"), "path", root) == NULL) {
        cJSON_Delete(source);
        return NULL;
    }
    return source;
}

static int add_claude_files(const entry_loader *loader, cJSON *changes, const char *root) {
    const char *marketplace_segments[3] = {root, ".claude-plugin", "marketplace.json"};
    const char *plugin_segments[3] = {root, ".claude-plugin", "plugin.json"};
    const char *hooks_segments[3] = {root, "hooks", "hooks.json"};
    return add_file(changes, joined(marketplace_segments, 3),
                    json_content(parsed("{\"name\": \"" MARKETPLACE_NAME "\", \"owner\": {\"name\": \"spisor\"}, "
                                        "\"description\": \"Starts the spisor host when Claude Code starts\", "
                                        "\"plugins\": [{\"name\": \"" PLUGIN_NAME "\", \"source\": \".\", "
                                        "\"description\": \"" PLUGIN_DESCRIPTION "\"}]}"))) &&
           add_file(changes, joined(plugin_segments, 3),
                    json_content(parsed("{\"name\": \"" PLUGIN_NAME "\", \"description\": \"" PLUGIN_DESCRIPTION "\"}"))) &&
           add_file(changes, joined(hooks_segments, 3), json_content(hooks_of(loader)));
}

static int claude_code_entry(const entry_loader *loader, cJSON *changes) {
    const char *entries_segments[2] = {loader->state, "entries"};
    const char *root_segments[3] = {loader->state, "entries", loader->id};
    char *root = joined(root_segments, 3);
    const char *manifest_segments[2] = {root, ".claude-plugin"};
    const char *hooks_segments[2] = {root, "hooks"};
    int built = root != NULL && add_directory(changes, joined(entries_segments, 2)) &&
                add_directory(changes, runtime_catalog_copy(root)) && add_directory(changes, joined(manifest_segments, 2)) &&
                add_directory(changes, joined(hooks_segments, 2)) && add_claude_files(loader, changes, root) &&
                add_directory(changes, runtime_catalog_copy(loader->home)) &&
                add_change(changes, settings_key(loader, "enabledPlugins", ENABLED_PLUGIN, cJSON_CreateTrue())) &&
                add_change(changes, settings_key(loader, "extraKnownMarketplaces", MARKETPLACE_NAME, marketplace_source(root)));
    free(root);
    return built;
}

/** A JSON string literal of text, which JavaScript reads back as the same string. */
static char *literal(const char *text) {
    cJSON *string = cJSON_CreateString(text);
    char *printed = string == NULL ? NULL : cJSON_PrintUnformatted(string);
    cJSON_Delete(string);
    char *copy = printed == NULL ? NULL : runtime_catalog_copy(printed);
    cJSON_free(printed);
    return copy;
}

/**
 * @implNote The host starts once per process however often OpenCode calls the plugin, and the plugin
 * returns at once, so a slow or refused start never holds OpenCode's own start; a refusal goes to
 * stderr, which OpenCode does not parse. The bundle is found through javascript-host's boot hint, so
 * the entry follows an update of javascript-host and never names a version a sweep can delete.
 */
static char *open_code_source(const entry_loader *loader) {
    const char *hint_segments[3] = {loader->cache, "hosts", RUNTIME_JAVASCRIPT_HOST_ID ".json"};
    const char *bundle_segments[3] = {loader->cache, "bundles", RUNTIME_JAVASCRIPT_HOST_ID};
    char *hint_path = joined(hint_segments, 3);
    char *bundle_path = joined(bundle_segments, 3);
    char *hint = hint_path == NULL ? NULL : literal(hint_path);
    char *bundles = bundle_path == NULL ? NULL : literal(bundle_path);
    char *loader_id = literal(loader->id);
    const char *parts[] = {
        "import { readFileSync } from \"node:fs\";\n",
        "import { join } from \"node:path\";\n",
        "import { pathToFileURL } from \"node:url\";\n\n",
        "const HINT = ", hint, ";\n",
        "const BUNDLES = ", bundles, ";\n",
        "const LOADER_ID = ", loader_id, ";\n",
        "const REINSTALL_FIX = \"" REINSTALL_FIX "\";\n\n",
        "let started;\n\n",
        "function hostEntry() {\n",
        "  const { host } = JSON.parse(readFileSync(HINT, \"utf8\"));\n",
        "  if (typeof host !== \"string\") throw new Error(`no javascript-host version is hinted in ${HINT}`);\n",
        "  return pathToFileURL(join(BUNDLES, host, \"host.js\")).href;\n",
        "}\n\n",
        "function reportRefusal(error) {\n",
        "  const detail = error?.detail ?? error?.message ?? String(error);\n",
        "  process.stderr.write(`spisor did not start ${LOADER_ID}: ${detail}\\nfix: ${error?.fix ?? REINSTALL_FIX}\\n`);\n",
        "}\n\n",
        "export const SpisorHost = async () => {\n",
        "  started ??= Promise.resolve()\n",
        "    .then(() => import(hostEntry()))\n",
        "    .then((host) => host.runSessionHost(LOADER_ID))\n",
        "    .catch(reportRefusal);\n",
        "  return {};\n",
        "};\n",
    };
    size_t count = sizeof parts / sizeof parts[0];
    char *source = hint == NULL || bundles == NULL || loader_id == NULL ? NULL : runtime_cache_text(parts, count);
    free(loader_id);
    free(bundles);
    free(hint);
    free(bundle_path);
    free(hint_path);
    return source;
}

static int open_code_entry(const entry_loader *loader, cJSON *changes) {
    const char *plugins_segments[2] = {loader->home, "plugins"};
    const char *name_parts[3] = {"spisor-", loader->id, ".js"};
    char *name = runtime_cache_text(name_parts, 3);
    const char *file_segments[3] = {loader->home, "plugins", name};
    int built = name != NULL && add_directory(changes, runtime_catalog_copy(loader->home)) &&
                add_directory(changes, joined(plugins_segments, 2)) &&
                add_file(changes, joined(file_segments, 3), open_code_source(loader));
    free(name);
    return built;
}

static entry_writer writer_for(const char *application) {
    if (strcmp(application, "claude") == 0) {
        return claude_code_entry;
    }
    return strcmp(application, "opencode") == 0 ? open_code_entry : NULL;
}

int runtime_entry_needs_javascript_host(const char *application_id) {
    return application_id != NULL && writer_for(application_id) == open_code_entry;
}

static cJSON *apply_call(const entry_loader *loader, const cJSON *change) {
    cJSON *call = runtime_install_places_call(loader->places);
    if (call == NULL || cJSON_AddStringToObject(call, "host", loader->id) == NULL ||
        !runtime_install_copy_into(call, "change", change)) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

static cJSON *applied(const entry_loader *loader, const cJSON *changes, cJSON **refusal) {
    cJSON *sequences = cJSON_CreateArray();
    for (const cJSON *change = sequences == NULL ? NULL : changes->child; change != NULL; change = change->next) {
        cJSON *answer = runtime_call_or_refuse(runtime_changes_apply_json, apply_call(loader, change), refusal);
        const cJSON *sequence = cJSON_GetObjectItemCaseSensitive(answer, "sequence");
        int kept = answer != NULL && (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answer, "recorded")) ||
                                      !cJSON_IsNumber(sequence) ||
                                      cJSON_AddItemToArray(sequences, cJSON_CreateNumber(sequence->valuedouble)));
        cJSON_Delete(answer);
        if (!kept) {
            cJSON_Delete(sequences);
            return NULL;
        }
    }
    return sequences;
}

static cJSON *written(cJSON *sequences) {
    cJSON *answer = sequences == NULL ? NULL : cJSON_CreateObject();
    if (answer == NULL || cJSON_AddTrueToObject(answer, "written") == NULL ||
        !cJSON_AddItemToObject(answer, "sequences", sequences)) {
        cJSON_Delete(sequences);
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static cJSON *not_written(const entry_loader *loader) {
    const char *message_parts[2] = {"this host writes no entry for the application ", loader->application};
    const char *fix_parts[3] = {"start ", loader->label, " through the command the loader requested"};
    char *message = runtime_cache_text(message_parts, 2);
    char *fix = runtime_cache_text(fix_parts, 3);
    cJSON *answer = message == NULL || fix == NULL ? NULL : cJSON_CreateObject();
    if (answer != NULL && (cJSON_AddFalseToObject(answer, "written") == NULL ||
                           cJSON_AddStringToObject(answer, "message", message) == NULL ||
                           cJSON_AddStringToObject(answer, "fix", fix) == NULL)) {
        cJSON_Delete(answer);
        answer = NULL;
    }
    free(fix);
    free(message);
    return answer;
}

static int read_loader(const cJSON *places, const cJSON *loader, entry_loader *read) {
    const cJSON *application = cJSON_GetObjectItemCaseSensitive(loader, "application");
    read->places = places;
    read->state = runtime_install_text(places, "state");
    read->cache = runtime_install_text(places, "cache");
    read->id = runtime_install_text(loader, "id");
    read->application = runtime_install_text(application, "id");
    read->label = runtime_install_text(application, "label");
    read->home = runtime_install_text(loader, "home");
    if (read->label == NULL) {
        read->label = read->application;
    }
    return read->state != NULL && spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, read->state) && read->cache != NULL &&
           spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, read->cache) && read->id != NULL && read->id[0] != 0 &&
           read->application != NULL && read->home != NULL && spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, read->home);
}

cJSON *runtime_install_entry(const cJSON *places, const cJSON *loader, cJSON **refusal) {
    entry_loader read;
    if (!read_loader(places, loader, &read)) {
        *refusal = runtime_refusal("the argument carries no usable places or loader", ENTRY_FIX);
        return NULL;
    }
    entry_writer writer = writer_for(read.application);
    if (writer == NULL) {
        return not_written(&read);
    }
    cJSON *changes = cJSON_CreateArray();
    cJSON *answer = changes != NULL && writer(&read, changes) ? written(applied(&read, changes, refusal)) : NULL;
    cJSON_Delete(changes);
    return answer;
}

static cJSON *entry_for(const cJSON *argument, cJSON **refusal) {
    return runtime_install_entry(cJSON_GetObjectItemCaseSensitive(argument, "places"),
                                 cJSON_GetObjectItemCaseSensitive(argument, "loader"), refusal);
}

RUNTIME_EXPORT int32_t runtime_install_entry_json(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, entry_for);
}
