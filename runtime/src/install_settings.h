#ifndef RUNTIME_INSTALL_SETTINGS_H
#define RUNTIME_INSTALL_SETTINGS_H

#include "cJSON.h"

/** The ecosystem settings one install reads and writes: the host's own home and the portable place. */
typedef struct runtime_install_settings {
    const char *home;
    const char *portable;
    const char *state;
} runtime_install_settings;

/**
 * The merged values of the layers named: both with portable set and home the host's, the application
 * layer alone with portable NULL, the portable layer alone with home NULL. NULL with *refusal set, or
 * with none when memory runs out.
 */
cJSON *runtime_install_settings_values(const runtime_install_settings *settings, int application, int portable,
                                       cJSON **refusal);

/** Writes key as value, which it frees, into layer ("application" or "portable"); 1 on success, else as above. */
int runtime_install_settings_set(const runtime_install_settings *settings, const char *key, cJSON *value,
                                 const char *layer, cJSON **refusal);

/**
 * Appends each of sources that catalog.sources does not list yet, never replacing one that is: a local
 * source to the application layer, which starts from the merged list, and an https source to the
 * portable layer and to the application layer too whenever that has a list of its own. 1 on success.
 *
 * @implNote A local source names a path on this machine, which the portable place never holds
 * (home-layout spec section 3). Layers merge a list whole, so an application list hides the portable
 * one, which is why both rules start from what the reader of the merged settings would see.
 */
int runtime_install_record_sources(const runtime_install_settings *settings, const cJSON *sources, cJSON **refusal);

#endif
