#ifndef RUNTIME_INSTALL_H
#define RUNTIME_INSTALL_H

#include <stddef.h>
#include <stdint.h>

#include "export.h"

/**
 * install.packages: { places: { portable }, item, manifestPackages? } answering
 * { packages: { "<method>": { name, options? } } }. A mapping is a package name or
 * { name, options? } and is answered in the object form. The manifest's mappings come first, then
 * the item's entry in <portable>/packages.json, whose mapping for a method replaces the manifest's
 * for that method only. An absent file adds nothing; a file that is unreadable, not an object, or
 * holds a mapping of another shape is refused with a fix naming it. Pure: nothing is written.
 */
RUNTIME_EXPORT int32_t runtime_install_packages(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/**
 * install.plan: { places: { portable }, item, manifestPackages?, methods, chosen, installedProviders?,
 * catalog? } answering { steps }, each step { "install-provider": <catalog entry> } or
 * { "install": { method, package? } }. The chosen method must be one of methods, except
 * "marketplace", which is built into every host and is always plannable with no package. Any other
 * method needs a package mapping (as install.packages merges them) and a provider: when chosen is
 * not in installedProviders, the first entry of catalog whose installMethod is chosen is installed
 * first, and none is a refusal naming the method. catalog is ordered by the caller, so earlier
 * marketplaces take precedence. Pure: a host executes the steps.
 */
RUNTIME_EXPORT int32_t runtime_install_plan(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
