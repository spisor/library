#ifndef RUNTIME_CONFIG_H
#define RUNTIME_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "runtime/settings.h"
#include "secrets.h"
#include "spisor/status.h"

/**
 * The name a plugin's config file and log carry: config.name when it is a non-empty string, else the
 * plugin id, then trimmed and checked as a portable path segment, in *name and freed: SPISOR_MALFORMED when the
 * result is not a usable segment, SPISOR_OUT_OF_MEMORY when memory runs out. config may be NULL.
 *
 * @implNote The emptiness test is before trimming, as the TypeScript || it replaced is, so a name of spaces is
 * refused rather than falling back to the id.
 */
spisor_status runtime_config_name(const char *plugin, const cJSON *config, char **name);

/**
 * The merged settings tree of one plugin, as a new tree freed with cJSON_Delete: config.defaults, then
 * portable/plugins/<plugin id>/config.json, then the file under the home's config directory, each
 * applied over the last. portable may be NULL, for a host with no portable layer. A file that is
 * absent, malformed or not an object contributes nothing. Reads the files on every call and creates
 * nothing. NULL with *failed set to 1 when a name is unusable or memory runs out.
 */
cJSON *runtime_read_settings(const char *home, const cJSON *names, const char *portable, const char *plugin,
                             const cJSON *config, int *failed);

/**
 * The merged ecosystem settings, as runtime_read_settings merges a plugin's: portable/settings.json,
 * then settings.json under the home's config directory. portable may be NULL. NULL with *failed set
 * to 1 when memory runs out.
 */
cJSON *runtime_read_ecosystem_settings(const char *home, const cJSON *names, const char *portable, int *failed);

/**
 * The settings file of the layer that answers key in the merged ecosystem settings of home: the portable
 * place's settings.json when only that layer holds key, else settings.json under the home's config
 * directory, the layer a user's choice is written to. portable may be NULL. Freed; NULL when
 * memory runs out. The file need not exist.
 */
char *runtime_ecosystem_settings_file_holding(const char *home, const char *portable, const char *key);

/**
 * One read or write of settings, the operations' arguments as runtime's C code holds them. `plugin` NULL reads the
 * ecosystem's own settings. `names`, `config` and `settings` are NULL for none; `settings` is an object holding each
 * declaration under its key as {type?, layer?}, a layer being "application" or "portable". `places` NULL is a host
 * with no secret store. `key` is what get and set read, `value` and `layer` (a runtime_settings_layer) what set
 * writes. `previous_tree`, an empty tree or NULL, is where set also answers the previous value as a tree, built
 * before it writes. `merges` 1 makes set merge `value` into what the layer's file holds at `key`, as runtime_merge
 * applies a file over defaults, instead of replacing it.
 */
typedef struct runtime_config_request {
    const char *home;
    const char *portable;
    const char *plugin;
    const cJSON *names;
    const cJSON *config;
    const cJSON *settings;
    const runtime_secret_places *places;
    const char *key;
    const cJSON *value;
    uint32_t layer;
    spisor_value_tree *previous_tree;
    int merges;
} runtime_config_request;

/** A request for the settings of home over portable, every other field NULL or the default layer. */
runtime_config_request runtime_config_request_of(const char *home, const char *portable);

/**
 * The config and settings operations with the rules, statuses and refusals include/runtime/config.h and settings.h
 * document, answering their JSON: `values` a new object, `value` and `previous` a new tree or NULL for none, `secret`
 * 1 when a set stored a secret setting.
 */
spisor_status runtime_config_all_values(const runtime_config_request *request, cJSON **values, spisor_refusal *refusal);
spisor_status runtime_config_get_value(const runtime_config_request *request, cJSON **value, spisor_refusal *refusal);
spisor_status runtime_config_set_value(const runtime_config_request *request, cJSON **previous, int *secret,
                                       spisor_refusal *refusal);

/**
 * The ecosystem settings of home over the portable place (NULL for none), as settings' all answers them, for runtime's
 * own C callers. NULL with *refusal set to the {message, fix} refusal, or left NULL when memory ran out.
 */
cJSON *runtime_ecosystem_settings_values(const char *home, const char *portable, cJSON **refusal);

/** Writes one ecosystem setting as settings' set writes it; 1, or 0 with *refusal set as above. */
int runtime_set_ecosystem_setting(const char *home, const char *portable, const char *key, const cJSON *value,
                                  uint32_t layer, cJSON **refusal);

/** As runtime_set_ecosystem_setting, merging value into what the layer holds at key in the one write. */
int runtime_merge_ecosystem_setting(const char *home, const char *portable, const char *key, const cJSON *value,
                                    uint32_t layer, cJSON **refusal);

#endif
