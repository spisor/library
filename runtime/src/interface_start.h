#ifndef RUNTIME_INTERFACE_START_H
#define RUNTIME_INTERFACE_START_H

#include "cJSON.h"

/** The word every interface item's entry takes first, then `<runtime library> [<loader>] [--window]`. */
#define RUNTIME_INTERFACE_WORD "ui"
#define RUNTIME_INTERFACE_WINDOW_FLAG "--window"

/**
 * How to start the interface the install in `environment` chose, as processes.hold takes a start: {executable,
 * arguments, environment}, the entry of the chosen interface item's bundle at the version its hint names, given
 * `ui <runtime binary> [<loader id>] [--window]`, the runtime binary the one that hint names, `--window` when `window`
 * is 1, in environment. The interface is the one spisor-host's `interface` setting names, else the catalog's only
 * interface item.
 *
 * @return the start; NULL with *refusal set when the install is headless (the setting says none or the catalog lists
 * no interface item), its fix naming the `interface` setting and the catalog's interface items; when the catalog lists
 * two or more and the setting names none; when the setting names an item the catalog lists as no interface; when
 * spisor-host is not registered or its settings or catalog cannot be read; and as runtime_layer_hint,
 * runtime_hinted_binary and runtime_entry_start refuse; NULL with it NULL when memory runs out.
 */
cJSON *runtime_interface_start(const cJSON *environment, const char *loader_id, int window, cJSON **refusal);

#endif
