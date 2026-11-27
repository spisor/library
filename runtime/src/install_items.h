#ifndef RUNTIME_INSTALL_ITEMS_H
#define RUNTIME_INSTALL_ITEMS_H

#include <stddef.h>

#include "cJSON.h"
#include "install_run.h"

#define RUNTIME_FOREBAY_LIBRARY_ID "forebay-library"

/**
 * What an install has settled so far, beside the run every step shares: the settings whose plugins.<id>.pin holds an
 * item to its version, and `running`, an object of the id and version of every item installed or found to run.
 */
typedef struct runtime_install_items {
    runtime_install_run *run;
    const cJSON *values;
    cJSON *running;
} runtime_install_items;

/**
 * Installs target {id, pin?} of kind, its version referenced for host_id first, and notes it in running; 0 only when
 * memory runs out, what could not be done being reported.
 */
int runtime_install_referenced(runtime_install_items *items, const char *host_id, const cJSON *target,
                               const char *kind);

/** Installs each of ids, of kind, at the pin values hold for it or the catalog's latest, for spisor-host; as above. */
int runtime_install_pinned(runtime_install_items *items, const char *const *ids, size_t count, const char *kind);

/**
 * Records the libraries, at the versions running holds, and the host version the next start of host_id boots, when
 * running holds that host; 0 only when memory runs out, a refused write being reported.
 */
int runtime_install_write_hint(runtime_install_items *items, const char *host_id, const char *const *libraries,
                               size_t count);

/**
 * Installs language's host item `<language>-host` for spisor-host and hints it with the runtime and forebay-library
 * running holds; 0 only when memory runs out.
 */
int runtime_install_language_host(runtime_install_items *items, const char *language);

/** Acquires the JavaScript runtime into the report's javascript; 0 only when memory runs out, a refusal reported. */
int runtime_install_acquire_javascript(runtime_install_run *run);

#endif
