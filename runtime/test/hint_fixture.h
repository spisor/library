#ifndef RUNTIME_TEST_HINT_FIXTURE_H
#define RUNTIME_TEST_HINT_FIXTURE_H

#include "cJSON.h"

/** A copy of hint with each library entry replaced by its version, for tests that pin versions alone; NULL for NULL. */
static inline cJSON *hinted_versions(const cJSON *hint) {
    cJSON *versions = hint == NULL ? NULL : cJSON_Duplicate(hint, 1);
    const cJSON *libraries = cJSON_GetObjectItemCaseSensitive(hint, "libraries");
    cJSON *pinned = libraries == NULL ? NULL : cJSON_CreateObject();
    for (const cJSON *library = pinned == NULL ? NULL : libraries->child; library != NULL; library = library->next) {
        const cJSON *version = cJSON_GetObjectItemCaseSensitive(library, "version");
        cJSON_AddStringToObject(pinned, library->string, cJSON_IsString(version) ? version->valuestring : "");
    }
    if (pinned != NULL) {
        cJSON_ReplaceItemInObjectCaseSensitive(versions, "libraries", pinned);
    }
    return versions;
}

#endif
