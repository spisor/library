#ifndef RUNTIME_INSTALL_ECOSYSTEM_H
#define RUNTIME_INSTALL_ECOSYSTEM_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "spisor/status.h"

/** Takes a finished report into `taken`: OK, or OUT_OF_MEMORY to stop the install before its last write. */
typedef spisor_status (*runtime_install_ecosystem_taker)(const cJSON *report, void *taken);

/**
 * What install's ecosystem installs from: sources, an array of catalog sources; places, { portable, state, cache };
 * environment, an object of variable names to values; choices, an object of application to method; defaults and
 * requested, arrays of plugin ids, or NULL; platform, a platform key, or NULL for this build's.
 */
typedef struct runtime_install_ecosystem_request {
    const cJSON *sources;
    const cJSON *places;
    const cJSON *environment;
    const cJSON *choices;
    const cJSON *defaults;
    const cJSON *requested;
    const char *platform;
} runtime_install_ecosystem_request;

/**
 * install's ecosystem: installs the ecosystem from catalog sources and answers the report
 * { installed, cached, unavailable, enabled, applications, loaders, missing, javascript? }.
 *
 * Every source is checked now. Each catalog application is settled into applications: installed or
 * detected as it is found, installed through the method choices names (or skipped for "skip"), missing
 * with its offered methods and the exact --application flag, listed again in missing as
 * { application, methods } for the caller to ask about and call again with choices, or unavailable
 * when no built-in method serves it. Then spisor-host and the runtime library install at their pins or
 * the catalog's latest, and the plugins the ecosystem settings enable, or defaults when they name none,
 * plus each of requested; every installed default and requested plugin is enabled in the portable
 * settings unless the user turned it off; each source not yet listed is recorded; spisor-host is
 * registered and its boot hint written. The loader of every installed or detected application is
 * installed and registered with its commands, which run spisor-boot, and its application entry, and
 * every background host a loader names is installed and registered. The language-host item
 * `<language>-host` of every language an installed plugin or a loader's required plugin is written in installs
 * beside forebay-library, hinted with the runtime and forebay-library, and a plugin whose language the catalog lists
 * no such item for is reported unavailable and not installed. The interface spisor-host's `interface` setting names
 * installs, hinted with the runtime, none for `none`, else the catalog's one item of kind interface, two or more with
 * none chosen reported unavailable under `interface` naming each, and none listed leaving the install headless. The
 * Node the language hosts pin is acquired (javascript) only when a language host or background host this install
 * would start runs on JavaScript.
 *
 * An item that cannot be installed is reported under unavailable and the rest still install. A choice
 * naming an unknown application, or a method that is not offered, is refused before anything is
 * installed.
 *
 * @implNote No install or download runs under the registry lock: each version is recorded as a
 * reference first under a short lock, then stored unlocked, so a sweep running meanwhile keeps
 * it and two installs at once end with one consistent registry and cache.
 *
 * Hands the finished report to `take` before the running versions are recorded, and again when that record's
 * refusal adds to it; NULL with no refusal when `take` runs out of memory.
 */
cJSON *runtime_install_ecosystem(const runtime_install_ecosystem_request *request,
                                 runtime_install_ecosystem_taker take, void *taken, cJSON **refusal);

/**
 * install's application: under places { portable, state, cache }, operation "update" or "remove"
 * updates or removes the application loader is registered for through the method its registration
 * recorded, and answers { operation, version? }; after an update the version the method reports is
 * recorded in the registration's application provenance. Refused, naming where the application came
 * from, when it was found on the PATH or how it was installed is not recorded, since only what an
 * install method installed is that method's to change; the loader stays registered either way.
 */
cJSON *runtime_install_operate_application(const cJSON *places, const char *loader, const char *operation,
                                           cJSON **refusal);

/**
 * install's registerLoader: under places { portable, state, cache } with environment, registers the
 * loader id at the installed version as install registers it, and answers what hosts.register answers:
 * its application read from that version's host.json, the provenance its registration records kept, and
 * its commands, which run spisor-boot, requested again. What every start of a loader does, a launch or
 * an application's own hook, with no catalog read.
 */
cJSON *runtime_install_register_loader(const cJSON *places, const cJSON *environment, const char *id,
                                       const char *version, cJSON **refusal);

#endif
