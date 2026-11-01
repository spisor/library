#ifndef RUNTIME_CONFIG_H
#define RUNTIME_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "export.h"

/**
 * The name a plugin's config file and log carry: config.name when it is a non-empty string, else the
 * plugin id, then trimmed and checked as a portable path segment. Freed with free; NULL when the
 * result is not a usable segment or memory runs out. config may be NULL.
 *
 * @implNote The emptiness test is before trimming, as basekit's || is, so a name of spaces is
 * refused rather than falling back to the id.
 */
char *runtime_config_name(const char *plugin, const cJSON *config);

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

RUNTIME_EXPORT int32_t runtime_config_all(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);
RUNTIME_EXPORT int32_t runtime_config_get(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);
RUNTIME_EXPORT int32_t runtime_config_set(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
