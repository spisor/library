#ifndef RUNTIME_CACHE_DEPLOY_H
#define RUNTIME_CACHE_DEPLOY_H

#include <stddef.h>
#include <stdint.h>

#include "export.h"

/**
 * cache.deploy: {directory, pluginDir, identity: {id, name}} puts the plugin installed in directory where a host loads
 * it from, as pluginDir/<importName>.json, pluginDir/<importName>.js when its plugin.json names an entry, and
 * pluginDir/<importName>_identity.json holding {id, name} as the argument gives them, and answers {importName}.
 *
 * @implNote The only writer of an identity file, and it takes the identity from its argument alone: nothing a bundle
 * states is read as who the plugin is, and a bundle file named like an identity file is never copied, since only the
 * manifest and the entry are. importName comes from the bundle's own plugin.json, so it is checked against the
 * import name grammar of plugin.schema.json, Windows reserved names included, before any path is formed, and
 * everything is checked before the first file is written, so a refusal writes nothing.
 */
RUNTIME_EXPORT int32_t runtime_cache_deploy(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
