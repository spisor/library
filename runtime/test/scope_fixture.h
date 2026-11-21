#ifndef RUNTIME_TEST_SCOPE_FIXTURE_H
#define RUNTIME_TEST_SCOPE_FIXTURE_H

#include <weivel/scope.h>

#include "runtime/standard.h"
#include "scope_host.h"
#include "service_scratch.h"

/**
 * A plugin whose defaults hold every kind a settings file can, a record two deep, and what is not offered: a null, a
 * list, a declared secret and members no dotted key reaches. Its application file overrides one text and one nested
 * number, and its declarations put `depth` in the portable layer and `theme` in the application layer.
 */
#define SCOPE_MANIFEST                                                                                              \
    "{\"id\": \"viewer\", \"config\": {\"name\": \"viewer\", \"defaults\": {\"theme\": \"dark\", \"depth\": 3, "    \
    "\"debug\": false, \"nothing\": null, \"list\": [1, 2], \"cache\": {\"enabled\": true, \"ttl\": 60, "            \
    "\"inner\": {\"deep\": \"x\"}}, \"token\": \"kept\", \"dot.ted\": 1, \"\": 2}}, "                                \
    "\"settings\": {\"theme\": {\"type\": \"choice\", \"layer\": \"application\"}, "                                \
    "\"depth\": {\"type\": \"number\", \"layer\": \"portable\"}, \"token\": {\"type\": \"secret\"}}}"
#define SCOPE_APPLICATION_FILE "{\"theme\": \"light\", \"cache\": {\"ttl\": 30}}"

/**
 * The places a scope test writes: the application's home, its config folder, the portable and cache places, the
 * secrets file and the manifest.
 */
typedef struct scope_place {
    char base[1024];
    char *home;
    char *config;
    char *portable;
    char *cache;
    char *secrets_file;
    char *manifest;
} scope_place;

static inline void scope_place_free(scope_place *place) {
    spisor_memory_release(place->home);
    spisor_memory_release(place->config);
    spisor_memory_release(place->portable);
    spisor_memory_release(place->cache);
    spisor_memory_release(place->secrets_file);
    spisor_memory_release(place->manifest);
    memset(place, 0, sizeof *place);
}

/** The host over a place, keeping its secrets in the file alone so no test reaches the OS credential store. */
static inline runtime_scope_host scope_host_of(const scope_place *place) {
    runtime_scope_host host;
    memset(&host, 0, sizeof host);
    host.home = place->home;
    host.portable = place->portable;
    host.cache = place->cache;
    host.secrets_file = place->secrets_file;
    host.force_file_store = 1;
    host.no_color = 1;
    return host;
}

/** Writes a manifest of `text` beside the place's own, answering its path, freed. */
static inline char *write_manifest(const scope_place *place, const char *name, const char *text) {
    return touch_file(place->base, name, text, 0644);
}

/** A fresh place with the fixture plugin's manifest and application file, and the host started over it. */
static inline void start_scope_place(scope_place *place, const char *suite, const char *label) {
    runtime_scope_host host;
    char *secrets;
    scratch_in(place->base, sizeof place->base, suite, label);
    make_directory_in(place->base, "home");
    make_directory_in(place->base, "portable");
    place->home = joined_path(place->base, "home");
    place->portable = joined_path(place->base, "portable");
    place->cache = joined_path(place->base, "cache");
    secrets = joined_path(place->base, "secrets");
    place->secrets_file = joined_path(secrets, "secrets.json");
    spisor_memory_release(secrets);
    make_directory_in(place->home, "config");
    place->config = joined_path(place->home, "config");
    spisor_memory_release(touch_file(place->config, "viewer.json", SCOPE_APPLICATION_FILE, 0644));
    place->manifest = write_manifest(place, "plugin.json", SCOPE_MANIFEST);
    host = scope_host_of(place);
    CHECK(runtime_scopes_begin(&host) == SPISOR_OK);
}

static inline const weivel_standard_scopes *scopes_definition(void) {
    return runtime_standard_scope_definition_of();
}

#endif
