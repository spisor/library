#include "host_front_door.h"
#include "allocation.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cache_refusal.h"
#include "config.h"
#include "hosts_layers.h"
#include "json_service.h"
#include "node_json_file.h"
#include "plugin_scope.h"
#include "registry.h"

#define SETTING_TARGET "setting:"
#define LOOPBACK_URL "http://127.0.0.1:"
#define NO_PORT_FIX_START "install a version of "
#define NO_PORT_FIX_END " that declares the port it listens on as port access with mode listen"

static const cJSON *member(const cJSON *object, const char *name) {
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

static const char *text_of(const cJSON *object, const char *name) {
    return cJSON_GetStringValue(member(object, name));
}

static int is_text(const cJSON *object, const char *name, const char *expected) {
    const char *text = text_of(object, name);
    return text != NULL && strcmp(text, expected) == 0;
}

static int is_port(const cJSON *value) {
    return cJSON_IsNumber(value) && value->valuedouble >= 1 && value->valuedouble <= 65535 &&
           floor(value->valuedouble) == value->valuedouble;
}

/**
 * Whether the run's layer holds its loader's front door.
 *
 * @implNote hosts.start answers the run's identity by runtime_run_identity, the loader id alone exactly for that layer.
 */
static int holds_front_door(const runtime_host_run *run) {
    return strcmp(run->run_id, run->host_id) == 0;
}

static const char *located_manifest(const runtime_host_run *run, const char *plugin) {
    const cJSON *found;
    cJSON_ArrayForEach(found, member(run->located, "located")) {
        if (is_text(found, "id", plugin)) {
            return text_of(found, "manifest");
        }
    }
    return NULL;
}

/** The target of the manifest's first `port` access with mode `listen`, NULL when it declares none. */
static const cJSON *listening_target(const cJSON *manifest) {
    const cJSON *access;
    cJSON_ArrayForEach(access, member(manifest, "access")) {
        if (is_text(access, "kind", "port") && is_text(access, "mode", "listen")) {
            return member(access, "target");
        }
    }
    return NULL;
}

/** A refusal naming plugin as the loader's front door, `reason` after it; NULL when memory runs out. */
static cJSON *front_door_refusal(const runtime_host_run *run, const char *plugin, const char *reason,
                                 const char *fix) {
    const char *parts[4] = {plugin, ", the front door of ", run->host_id, reason};
    return runtime_cache_refusal(parts, 4, fix);
}

static cJSON *no_port_refusal(const runtime_host_run *run, const char *plugin) {
    const char *fix_parts[3] = {NO_PORT_FIX_START, plugin, NO_PORT_FIX_END};
    char *fix = runtime_cache_text(fix_parts, 3);
    cJSON *refusal = fix == NULL ? NULL
                                 : front_door_refusal(run, plugin,
                                                      ", declares no fixed port it listens on, so its application "
                                                      "cannot be handed the address",
                                                      fix);
    spisor_memory_release(fix);
    return refusal;
}

/** Sets *value to what the merged settings of the plugin at manifest hold at key, as its scope's config reads them. */
static spisor_status setting_of(const char *manifest, const char *key, cJSON **value, spisor_refusal *refusal) {
    runtime_plugin_scope *scope = NULL;
    spisor_status status = runtime_plugin_scope_open(manifest, &scope, refusal);
    if (status == SPISOR_OK) {
        runtime_config_request request = runtime_plugin_scope_request(scope);
        request.key = key;
        status = runtime_config_get_value(&request, value, refusal);
    }
    runtime_plugin_scope_release(scope);
    return status;
}

/** A refusal of a setting that holds no port, naming the setting and why it could not be read when it could not. */
static cJSON *unusable_setting_refusal(const runtime_host_run *run, const char *plugin, const char *key,
                                       const char *detail) {
    const char *reason_parts[4] = {", listens on the port its setting ", key,
                                   detail == NULL ? " holds, which is no port from 1 to 65535"
                                                  : " holds, which cannot be read: ",
                                   detail == NULL ? "" : detail};
    const char *fix_parts[4] = {"set ", key, " of ", plugin};
    char *reason = runtime_cache_text(reason_parts, 4);
    char *fix_start = runtime_cache_text(fix_parts, 4);
    const char *fix_whole[2] = {fix_start, " to a port from 1 to 65535"};
    char *fix = fix_start == NULL ? NULL : runtime_cache_text(fix_whole, 2);
    cJSON *refusal = reason == NULL || fix == NULL ? NULL : front_door_refusal(run, plugin, reason, fix);
    spisor_memory_release(fix);
    spisor_memory_release(fix_start);
    spisor_memory_release(reason);
    return refusal;
}

/**
 * The port the front door `plugin`, whose manifest is at `path`, declares it listens on, or 0 with *refusal set when it
 * declares no fixed one or its setting holds none, or with it NULL when memory runs out.
 */
static int declared_port(const runtime_host_run *run, const char *plugin, const char *path, cJSON **refusal) {
    int out_of_memory = 0;
    cJSON *manifest = runtime_read_node_json(path, &out_of_memory);
    const cJSON *target = listening_target(manifest);
    const char *setting = cJSON_GetStringValue(target);
    int is_setting = setting != NULL && strncmp(setting, SETTING_TARGET, strlen(SETTING_TARGET)) == 0 &&
                     setting[strlen(SETTING_TARGET)] != '\0';
    int port = 0;
    if (is_port(target)) {
        port = (int)target->valuedouble;
    } else if (is_setting) {
        const char *key = setting + strlen(SETTING_TARGET);
        spisor_refusal refused = SPISOR_INIT(spisor_refusal);
        cJSON *value = NULL;
        spisor_status status = setting_of(path, key, &value, &refused);
        port = status == SPISOR_OK && is_port(value) ? (int)value->valuedouble : 0;
        if (port == 0 && status != SPISOR_OUT_OF_MEMORY) {
            *refusal = unusable_setting_refusal(run, plugin, key, status == SPISOR_OK ? NULL : refused.detail);
        }
        cJSON_Delete(value);
        spisor_refusal_release(&refused);
    } else if (!out_of_memory) {
        *refusal = no_port_refusal(run, plugin);
    }
    cJSON_Delete(manifest);
    return port;
}

/** Sets the proxy's base URL to the loopback address at port and its placeholder; 0 when memory runs out. */
static int add_proxy_variables(cJSON *environment, const cJSON *proxy, int port) {
    const cJSON *placeholder = member(proxy, "placeholder");
    char base_url[sizeof LOOPBACK_URL + 8];
    snprintf(base_url, sizeof base_url, LOOPBACK_URL "%d", port);
    return cJSON_AddStringToObject(environment, text_of(proxy, "baseUrlVariable"), base_url) != NULL &&
           cJSON_AddStringToObject(environment, text_of(placeholder, "variable"), text_of(placeholder, "value")) !=
               NULL;
}

/** The proxy's variables for the front door the standard requires first, or NULL as the public function answers. */
static cJSON *proxy_environment(const runtime_host_run *run, const cJSON *standard, cJSON **refusal) {
    const cJSON *proxy = member(member(standard, "application"), "proxy");
    const char *plugin = cJSON_GetStringValue(cJSON_GetArrayItem(member(standard, "plugins"), 0));
    const char *path = plugin == NULL ? NULL : located_manifest(run, plugin);
    int port = path == NULL ? 0 : declared_port(run, plugin, path, refusal);
    cJSON *environment = port == 0 ? NULL : cJSON_CreateObject();
    if (plugin == NULL) {
        const char *parts[2] = {run->host_id, "'s standard declares a proxy but requires no plugin to serve it"};
        *refusal = runtime_cache_refusal(parts, 2, RUNTIME_REINSTALL_FIX);
    } else if (path == NULL) {
        *refusal = front_door_refusal(run, plugin,
                                      ", is not installed, so its application cannot be handed the address",
                                      RUNTIME_REINSTALL_FIX);
    }
    if (environment != NULL && !add_proxy_variables(environment, proxy, port)) {
        cJSON_Delete(environment);
        environment = NULL;
    }
    return environment;
}

cJSON *runtime_front_door_environment(const runtime_host_run *run, cJSON **refusal) {
    const cJSON *identity = member(run->started, "identity");
    const char *cache = text_of(member(run->started, "places"), "cache");
    int out_of_memory = 0;
    cJSON *standard = holds_front_door(run) ? runtime_loader_standard(cache, run->host_id, text_of(identity, "version"),
                                                                      &out_of_memory)
                                            : NULL;
    cJSON *environment;
    *refusal = NULL;
    if (out_of_memory) {
        return NULL;
    }
    environment = cJSON_IsObject(member(member(standard, "application"), "proxy"))
                      ? proxy_environment(run, standard, refusal)
                      : cJSON_CreateObject();
    cJSON_Delete(standard);
    return environment;
}
