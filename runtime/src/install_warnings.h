#ifndef RUNTIME_INSTALL_WARNINGS_H
#define RUNTIME_INSTALL_WARNINGS_H

#include "cJSON.h"

/**
 * install's warnings: answers {warnings: [{kind, subject, detail}]}, what a plugin's manifest depends on
 * by a name instead of an id. kind is "named-dependency" for a dependencies entry with a name and no id, subject
 * being the name, and "unbound-service" for a consumed service whose id is namespaced and names no provider,
 * subject being the service id. A dependency by id and a bare service id, which is shared, are never reported.
 * detail is one sentence saying why: a rename can break it and whoever takes that name can replace it.
 *
 * @implNote The service rule is that of unboundServices in weivel's identity.ts: an id containing ":" is namespaced.
 */
cJSON *runtime_install_warnings(const cJSON *manifest, cJSON **refusal);

#endif
