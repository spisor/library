#ifndef RUNTIME_INSTALL_ENTRIES_H
#define RUNTIME_INSTALL_ENTRIES_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "export.h"

/** The catalog item an entry declaring runsOnJavascriptHost runs a loader's host on. */
#define RUNTIME_JAVASCRIPT_HOST_ID "javascript-host"

/**
 * Writes the entry the loader's application declares, every directory, file and settings member through
 * changes.apply under the loader's id, so removing the loader reverses all of it. Answers {written: true,
 * runsOnJavascriptHost, sequences}, the sequence of each change recorded now, or {written: false, message,
 * fix} for an application declaring no entry; NULL with *refusal set when a change is refused.
 */
cJSON *runtime_install_entry(const cJSON *places, const cJSON *loader, cJSON **refusal);

/**
 * install.entry: { places: { state, cache }, loader: { id, application: { id, label, entry }, home } }
 * answering what runtime_install_entry answers.
 *
 * @implNote A json-key change sets its member wholesale, so an entry must declare members it owns outright
 * rather than one the user also writes to. A change already on disk is not recorded again, so writing the
 * entry twice changes nothing.
 */
RUNTIME_EXPORT int32_t runtime_install_entry_json(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
