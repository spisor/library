#ifndef RUNTIME_CACHE_DEPLOY_H
#define RUNTIME_CACHE_DEPLOY_H

#include "cJSON.h"

/** Where cache's deploy puts the plugin installed in directory, and the identity it records for it. */
typedef struct runtime_cache_deploy_request {
    const char *directory;
    const char *plugin_directory;
    const char *identity_id;
    const char *identity_name;
    const char *replaces;
} runtime_cache_deploy_request;

/**
 * cache's deploy: puts the plugin installed in directory where a host loads
 * it from, as pluginDir/<importName>.json, pluginDir/<importName>.js when its plugin.json names an entry, and
 * pluginDir/<importName>_identity.json holding {id, name} as the request gives them; 1, or 0 with *refusal set, or
 * with it NULL when memory runs out.
 * replaces, when given, is an id whose deployed plugin of this import name this one may replace; no other identity
 * already deployed under the name is replaced.
 *
 * @implNote The only writer of an identity file, and it takes the identity from its request alone: nothing a bundle
 * states is read as who the plugin is, and a bundle file named like an identity file is never copied, since only the
 * manifest and the entry are. importName comes from the bundle's own plugin.json, so it is checked against the
 * import name grammar of plugin.schema.json, Windows reserved names included, before any path is formed, and
 * everything is checked before the first file is written, so a refusal writes nothing.
 */
int runtime_cache_deploy(const runtime_cache_deploy_request *request, cJSON **refusal);

/** A deploy checked and ready to write, so a caller can build its answer before the first file is written. */
typedef struct runtime_cache_deployment runtime_cache_deployment;

/** Checks everything cache's deploy checks, writing nothing: the plan, or NULL with *refusal set, NULL with it NULL out of memory. */
runtime_cache_deployment *runtime_cache_plan_deployment(const runtime_cache_deploy_request *request,
                                                        cJSON **refusal);

const char *runtime_cache_deployment_import_name(const runtime_cache_deployment *plan);

/** Writes the planned files: 1, or 0 with *refusal set, or with it NULL when memory ran out. */
int runtime_cache_carry_out_deployment(runtime_cache_deployment *plan, cJSON **refusal);

void runtime_cache_free_deployment(runtime_cache_deployment *plan);

#endif
