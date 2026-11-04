#ifndef RUNTIME_INSTALL_ENTRIES_H
#define RUNTIME_INSTALL_ENTRIES_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "export.h"

/** The catalog item OpenCode's in-process entry runs a loader's host on. */
#define RUNTIME_JAVASCRIPT_HOST_ID "javascript-host"

/** Whether the entry of application_id runs inside the application on the javascript-host bundle. */
int runtime_entry_needs_javascript_host(const char *application_id);

/**
 * Writes the entry that makes loader's application start the loader's host through its own plugin
 * mechanism, every file, directory and settings member through changes.apply under the loader's id, so
 * removing the loader reverses all of it. Answers {written: true, sequences}, the sequence of each change
 * recorded now, or {written: false, message, fix} for an application this runtime writes no entry for;
 * NULL with *refusal set when a change is refused.
 */
cJSON *runtime_install_entry(const cJSON *places, const cJSON *loader, cJSON **refusal);

/**
 * install.entry: { places: { state, cache }, loader: { id, application: { id, label }, home } } answering
 * what runtime_install_entry answers. Claude Code's entry is a directory marketplace in
 * <state>/entries/<loader id> whose one plugin runs forebay-boot on SessionStart, and the two settings
 * members that make Claude Code know and enable it; OpenCode's is one plugin file in its global plugin
 * directory that runs the loader's host on the javascript-host bundle in OpenCode's own process.
 *
 * @implNote Only extraKnownMarketplaces.spisor and enabledPlugins["spisor@spisor"] are written into
 * Claude Code's settings, never hooks: a json-key change sets its member wholesale, so setting
 * hooks.SessionStart would replace the user's own hooks. A change already on disk is not recorded again,
 * so writing the entry twice changes nothing.
 */
RUNTIME_EXPORT int32_t runtime_install_entry_json(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
