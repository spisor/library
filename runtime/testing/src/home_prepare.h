#ifndef RUNTIME_TESTING_HOME_PREPARE_H
#define RUNTIME_TESTING_HOME_PREPARE_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "home_layout.h"
#include "runtime_testing/home.h"

/** One install, checked: its manifest and settings parsed, and every path it writes resolved, `entry` NULL for none. */
typedef struct runtime_testing_prepared_install {
    cJSON *manifest;
    cJSON *settings;
    const char *id;
    const char *version;
    const char *entry_source;
    char *bundle;
    char *entry;
    char *settings_file;
} runtime_testing_prepared_install;

/** A home argument, checked before anything is written, so a malformed one writes nothing. */
typedef struct runtime_testing_prepared_home {
    const char *base;
    const char *host_id;
    int64_t holder;
    runtime_testing_layout layout;
    runtime_testing_prepared_install *installs;
    size_t install_count;
    cJSON *libraries;
} runtime_testing_prepared_home;

/**
 * Checks `argument` and resolves every path the home will hold into `home`, released with
 * runtime_testing_free_prepared_home whatever it answers.
 *
 * @return OK; MALFORMED with the refusal lay_out_home documents; OUT_OF_MEMORY.
 */
/**
 * Checks one install and resolves the paths it writes below layout into `prepared`, released with
 * runtime_testing_free_prepared_install whatever it answers. With `requires_entry` 0, a manifest naming no entry beside
 * an absent entry source is taken, as a bundle holding its manifest alone.
 *
 * @return OK; MALFORMED with the refusal lay_out_home documents; OUT_OF_MEMORY.
 */
spisor_status runtime_testing_prepare_install(const runtime_testing_install *install,
                                              const runtime_testing_layout *layout, int requires_entry,
                                              runtime_testing_prepared_install *prepared, spisor_refusal *refusal);

void runtime_testing_free_prepared_install(runtime_testing_prepared_install *prepared);

spisor_status runtime_testing_prepare_home(const runtime_testing_home_argument *argument,
                                           runtime_testing_prepared_home *home, spisor_refusal *refusal);

void runtime_testing_free_prepared_home(runtime_testing_prepared_home *home);

#endif
