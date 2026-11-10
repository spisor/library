#ifndef RUNTIME_INSTALL_ECOSYSTEM_H
#define RUNTIME_INSTALL_ECOSYSTEM_H

#include <stddef.h>
#include <stdint.h>

#include "export.h"

/**
 * install.ecosystem: { sources, places: { portable, state, cache }, environment, choices?, defaults?,
 * requested?, platform? } installs the ecosystem from catalog sources and answers the report
 * { installed, cached, unavailable, enabled, applications, loaders, missing, javascript? }.
 *
 * Every source is checked now. Each catalog application is settled into applications: installed or
 * detected as it is found, installed through the method choices names (or skipped for "skip"), missing
 * with its offered methods and the exact --application flag, listed again in missing as
 * { application, methods } for the caller to ask about and call again with choices, or unavailable
 * when no built-in method serves it. Then forebay-host and the runtime library install at their pins or
 * the catalog's latest, and the plugins the ecosystem settings enable, or defaults when they name none,
 * plus each of requested; every installed default and requested plugin is enabled in the portable
 * settings unless the user turned it off; each source not yet listed is recorded; forebay-host is
 * registered and its boot hint written. The loader of every installed or detected application is
 * installed and registered with its commands, which run forebay-boot, and its application entry, and
 * every background host a loader names is installed and registered. koffi, forebay-library and the
 * javascript-host bundle install only when something runs on them, forebay-library named in the
 * javascript-host's boot hint beside the runtime and koffi, and the pinned Node is acquired
 * (javascript) only when a plugin or background host this install would start runs on JavaScript.
 *
 * An item that cannot be installed is reported under unavailable and the rest still install. A choice
 * naming an unknown application, or a method that is not offered, is refused before anything is
 * installed.
 *
 * @implNote No install or download runs under the registry lock: each version is recorded as a
 * reference first under a short lock, then stored unlocked, so a sweep running meanwhile keeps
 * it and two installs at once end with one consistent registry and cache.
 */
RUNTIME_EXPORT int32_t runtime_install_ecosystem(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/**
 * install.application: { places: { portable, state, cache }, loader, operation: "update" | "remove" }
 * updates or removes the application loader is registered for through the method its registration
 * recorded, and answers { operation, version? }; after an update the version the method reports is
 * recorded in the registration's application provenance. Refused, naming where the application came
 * from, when it was found on the PATH or how it was installed is not recorded, since only what an
 * install method installed is that method's to change; the loader stays registered either way.
 */
RUNTIME_EXPORT int32_t runtime_install_application(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/**
 * install.registerLoader: { places: { portable, state, cache }, environment, id, version } registers the
 * loader id at the installed version as install registers it, and answers what hosts.register answers:
 * its application read from that version's host.json, the provenance its registration records kept, and
 * its commands, which run forebay-boot, requested again. What every start of a loader does, a launch or
 * an application's own hook, with no catalog read.
 */
RUNTIME_EXPORT int32_t runtime_install_register_loader(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
