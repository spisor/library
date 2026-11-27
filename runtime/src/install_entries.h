#ifndef RUNTIME_INSTALL_ENTRIES_H
#define RUNTIME_INSTALL_ENTRIES_H

#include "cJSON.h"

/** The library id runtime itself is installed and hinted under. */
#define RUNTIME_LIBRARY_ID "runtime"

/**
 * Writes the entry the loader's application declares, every directory, file and settings member through
 * changes.apply under the loader's id, so removing the loader reverses all of it, its {{hint}} and {{bundles}} standing
 * for the boot hint and the bundles of `<language>-host`, the loader's `language` being its front door's. Answers
 * {written: true, runsInProcess, sequences}, the sequence of each change recorded now, or {written: false, message,
 * fix} for an application declaring no entry; NULL with *refusal set when a change is refused, or when the loader
 * names no language and its entry runs in process or names {{hint}} or {{bundles}}.
 */
cJSON *runtime_install_entry(const cJSON *places, const cJSON *loader, cJSON **refusal);

#endif
