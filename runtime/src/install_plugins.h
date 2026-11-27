#ifndef RUNTIME_INSTALL_PLUGINS_H
#define RUNTIME_INSTALL_PLUGINS_H

#include "cJSON.h"

/**
 * What install's plugins installs: places, {portable, state, cache}; environment, an object of variable names to
 * values; host_id, the host whose settings enable the plugins; plugins, a non-empty array of catalog plugin ids.
 */
typedef struct runtime_install_plugins_request {
    const cJSON *places;
    const cJSON *environment;
    const char *host_id;
    const cJSON *plugins;
} runtime_install_plugins_request;

/**
 * install's plugins: installs the plugins for one host and answers the report {installed, cached, unavailable,
 * enabled, javascript?}, as runtime_install_definition's plugins describes it. NULL with *refusal set to {message,
 * fix}, having written nothing, for a host whose layers cannot be read and a plugin the catalog lists as no plugin, not
 * at all, or in a language it lists no host for; NULL with none when memory runs out.
 */
cJSON *runtime_install_plugins(const runtime_install_plugins_request *request, cJSON **refusal);

#endif
