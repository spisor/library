#include "install_hooks.h"

#include <stdlib.h>
#include <string.h>

#include "cache_refusal.h"
#include "install_support.h"
#include "node_json_file.h"
#include "paths.h"
#include "spisor/file.h"
#include "spisor/path.h"

#define INSTALLED_VERSION "1.0.0"
#define UPDATED_VERSION "2.0.0"

static char *record_path(const cJSON *places, int folder_only) {
    const char *segments[4] = {runtime_install_text(places, "cache"), "methods", "fake", "installed.json"};
    return segments[0] == NULL ? NULL : spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, folder_only ? 3 : 4);
}

static cJSON *read_installed(const cJSON *places) {
    char *path = record_path(places, 0);
    cJSON *installed = path == NULL ? NULL : runtime_read_node_json(path);
    free(path);
    if (!cJSON_IsObject(installed)) {
        cJSON_Delete(installed);
        installed = cJSON_CreateObject();
    }
    return installed;
}

static int write_installed(const cJSON *places, const cJSON *installed) {
    char *folder = record_path(places, 1);
    char *path = record_path(places, 0);
    char *text = cJSON_PrintUnformatted(installed);
    int written = folder != NULL && path != NULL && text != NULL && spisor_make_directories(folder) == 1 &&
                  spisor_replace_file(path, (const uint8_t *)text, strlen(text)) == 1;
    cJSON_free(text);
    free(path);
    free(folder);
    return written;
}

static cJSON *detect(const cJSON *places, const cJSON *mapped, cJSON **refusal) {
    (void)refusal;
    cJSON *installed = read_installed(places);
    const char *version = runtime_install_text(installed, runtime_install_text(mapped, "name"));
    cJSON *answer = cJSON_CreateObject();
    if (answer != NULL && (cJSON_AddBoolToObject(answer, "installed", version != NULL) == NULL ||
                           (version != NULL && cJSON_AddStringToObject(answer, "version", version) == NULL))) {
        cJSON_Delete(answer);
        answer = NULL;
    }
    cJSON_Delete(installed);
    return answer;
}

static int change(cJSON *installed, const char *operation, const char *package, cJSON **refusal) {
    cJSON_bool present = cJSON_HasObjectItem(installed, package);
    if (strcmp(operation, "remove") == 0) {
        cJSON_DeleteItemFromObjectCaseSensitive(installed, package);
        return 1;
    }
    if (strcmp(operation, "install") == 0) {
        return present || cJSON_AddStringToObject(installed, package, INSTALLED_VERSION) != NULL;
    }
    if (!present) {
        const char *parts[2] = {package, " is not installed"};
        const char *fix_parts[3] = {"install ", package, " first"};
        char *fix = runtime_cache_text(fix_parts, 3);
        *refusal = fix == NULL ? NULL : runtime_cache_refusal(parts, 2, fix);
        free(fix);
        return 0;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(installed, package);
    return cJSON_AddStringToObject(installed, package, UPDATED_VERSION) != NULL;
}

static int operate(const cJSON *places, const char *operation, const cJSON *mapped, cJSON **refusal) {
    const char *package = runtime_install_text(mapped, "name");
    cJSON *installed = read_installed(places);
    int done = installed != NULL && package != NULL && change(installed, operation, package, refusal) &&
               write_installed(places, installed);
    cJSON_Delete(installed);
    return done;
}

const runtime_install_method runtime_install_fake_method = {"fake", detect, operate};
