#ifndef RUNTIME_INSTALL_H
#define RUNTIME_INSTALL_H

/**
 * @file
 * install's operations over JSON, for runtime's own callers: each answers its result, or NULL with *refusal set to
 * {message, fix}, or with it NULL when memory runs out.
 */

#include "cJSON.h"

/** Whose packages install's packages merges: the marketplace item under the portable place, and its manifest's. */
typedef struct runtime_install_packages_request {
    const char *portable;
    const char *item;
    const cJSON *manifest_packages;
} runtime_install_packages_request;

/**
 * install's packages: answering { packages: { "<method>": { name, options? } } }; manifest_packages may be NULL. A
 * mapping is a package name or
 * { name, options? } and is answered in the object form. The manifest's mappings come first, then
 * the item's entry in <portable>/packages.json, whose mapping for a method replaces the manifest's
 * for that method only. An absent file adds nothing; a file that is unreadable, not an object, or
 * holds a mapping of another shape is refused with a fix naming it. Pure: nothing is written.
 */
cJSON *runtime_install_packages(const runtime_install_packages_request *request, cJSON **refusal);

/**
 * What install's plan plans: the chosen method among methods, an array of method ids, for the item's packages,
 * with installed_providers, an array of method ids, and catalog, an array of catalog entries, each NULL for none.
 */
typedef struct runtime_install_plan_request {
    runtime_install_packages_request packages;
    const char *chosen;
    const cJSON *methods;
    const cJSON *installed_providers;
    const cJSON *catalog;
} runtime_install_plan_request;

/**
 * install's plan: answering { steps }, each step { "install-provider": <catalog entry> } or
 * { "install": { method, package? } }. The chosen method must be one of methods, except
 * "marketplace", which is built into every host and is always plannable with no package. Any other
 * method needs a package mapping (as install's packages merges them) and a provider: when chosen is
 * not in installedProviders, the first entry of catalog whose installMethod is chosen is installed
 * first, and none is a refusal naming the method. catalog is ordered by the caller, so earlier
 * marketplaces take precedence. Pure: a host executes the steps.
 */
cJSON *runtime_install_plan(const runtime_install_plan_request *plan, cJSON **refusal);

#endif
