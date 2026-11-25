#ifndef RUNTIME_INSTALL_ENTRIES_H
#define RUNTIME_INSTALL_ENTRIES_H

#include "cJSON.h"

/** The catalog item an entry declaring runsOnJavascriptHost runs a loader's host on. */
#define RUNTIME_JAVASCRIPT_HOST_ID "javascript-host"

/** The catalog item whose boot hint names the native layer's libraries and version. */
#define RUNTIME_NATIVE_HOST_ID "spisor-interface"

/** The library id runtime itself is installed and hinted under. */
#define RUNTIME_LIBRARY_ID "runtime"

/** The library id koffi, the JavaScript host's FFI, is installed and hinted under. */
#define RUNTIME_KOFFI_LIBRARY_ID "koffi"

/**
 * Writes the entry the loader's application declares, every directory, file and settings member through
 * changes.apply under the loader's id, so removing the loader reverses all of it. Answers {written: true,
 * runsOnJavascriptHost, sequences}, the sequence of each change recorded now, or {written: false, message,
 * fix} for an application declaring no entry; NULL with *refusal set when a change is refused.
 */
cJSON *runtime_install_entry(const cJSON *places, const cJSON *loader, cJSON **refusal);

#endif
