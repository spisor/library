#ifndef RUNTIME_TEST_HOST_PROVENANCE_H
#define RUNTIME_TEST_HOST_PROVENANCE_H

#include "cJSON.h"

/** Adds a complete provenance to registration: every member registry.register requires. */
static inline void add_host_provenance(cJSON *registration, const char *method, const char *version,
                                       const char *location) {
    cJSON *provenance = cJSON_AddObjectToObject(registration, "provenance");
    cJSON_AddStringToObject(provenance, "method", method);
    cJSON_AddStringToObject(provenance, "version", version);
    cJSON_AddStringToObject(provenance, "location", location);
    cJSON_AddStringToObject(provenance, "os", "linux");
    cJSON_AddStringToObject(provenance, "architecture", "x64");
    cJSON_AddStringToObject(provenance, "installedAt", "2026-11-02T10:00:00Z");
}

#endif
