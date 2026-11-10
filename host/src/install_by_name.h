#ifndef HOST_INSTALL_BY_NAME_H
#define HOST_INSTALL_BY_NAME_H

#include "host_context.h"

/**
 * Installs the plugin name means, a bare name or marketplace/name, out of the marketplaces the portable settings list,
 * and prints what install.byId answered and then the warnings install.warnings finds in the plugin's manifest. The exit
 * code: 0 done, 1 refused. Installs nothing when the name is found nowhere, is found under two ids, or is a name this
 * host remembers as another id, and says which.
 *
 * accepted_id, when not NULL, is the id the user accepted in place of the one remembered, or of the primary the
 * repo's copies now name; it changes nothing unless it is the id now offered.
 *
 * @implNote The plugin is deployed into <portable>/plugins, the one place the runtime already names for plugins.
 * The name is remembered before the install, so a name that points elsewhere later is caught even if this install
 * fails; a remembered id is only ever replaced by an accepted one.
 */
int host_install_by_name(const host_context *context, const char *name, const char *accepted_id);

#endif
