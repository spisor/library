#include "marketplace_copies.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache_refusal.h"
#include "directory_tree.h"
#include "host_output.h"
#include "host_service.h"
#include "install_settings.h"
#include "marketplace_read.h"
#include "marketplace_sources.h"
#include "paths.h"
#include "spisor/file.h"
#include "spisor/path.h"

#define FETCH_FOLDER_SIZE 64
#define COPY_FILE "copy.json"

static void print_note(const char *subject, const char *detail) {
    char *safe_subject = host_printable(subject == NULL ? "" : subject);
    char *safe_detail = host_printable(detail == NULL ? "" : detail);
    host_write(1, "skipped ");
    host_write(1, safe_subject == NULL ? "" : safe_subject);
    host_write(1, ": ");
    host_write(1, safe_detail == NULL ? "" : safe_detail);
    host_write(1, "\n");
    free(safe_detail);
    free(safe_subject);
}

static void print_skipped_entries(const cJSON *skipped) {
    const cJSON *entry;
    cJSON_ArrayForEach(entry, skipped) {
        char subject[FETCH_FOLDER_SIZE];
        snprintf(subject, sizeof subject, "marketplaces[%d]", (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(entry, "index")));
        print_note(subject, host_text(entry, "detail"));
    }
}

static void print_source_refusal(const char *source_id, const cJSON *refusal) {
    char *safe_id = host_printable(source_id);
    host_write(1, "marketplace ");
    host_write(1, safe_id == NULL ? "" : safe_id);
    host_write(1, " could not be read, so its names are not searched: ");
    host_print_refusal_printably(refusal);
    free(safe_id);
}

static cJSON *listed_sources(const host_context *context, cJSON **refusal) {
    runtime_install_settings settings = {NULL, context->portable, context->state};
    cJSON *values = runtime_install_settings_values(&settings, 0, 1, refusal);
    cJSON *argument = values == NULL ? NULL : cJSON_CreateObject();
    if (argument == NULL || !cJSON_AddItemToObject(argument, "settings", values)) {
        cJSON_Delete(argument == NULL ? values : NULL);
        cJSON_Delete(argument);
        return NULL;
    }
    return host_call(runtime_marketplace_sources, argument, refusal);
}

static char *fetch_folder(const host_context *context, int index) {
    char name[FETCH_FOLDER_SIZE];
    snprintf(name, sizeof name, "marketplace-fetch-%ld-%d", (long)host_process_id(), index);
    const char *segments[2] = {context->cache, name};
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
}

static cJSON *read_call(const cJSON *source, const char *destination) {
    cJSON *argument = cJSON_CreateObject();
    if (argument == NULL || !cJSON_AddItemToObject(argument, "source", cJSON_Duplicate(source, 1)) ||
        cJSON_AddStringToObject(argument, "destination", destination) == NULL) {
        cJSON_Delete(argument);
        return NULL;
    }
    return argument;
}

/** The copy source serves, fetched into a folder made and removed here; NULL with *refusal set. */
static cJSON *fetched_copy(const host_context *context, const cJSON *source, int index, cJSON **refusal) {
    char *folder = fetch_folder(context, index);
    const char *segments[2] = {folder, COPY_FILE};
    char *destination = folder == NULL ? NULL : spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
    cJSON *copy = NULL;
    if (destination != NULL && runtime_remove_tree(folder) == 1 && spisor_make_directories(folder) == 1) {
        cJSON *answer = host_call(runtime_marketplace_read, read_call(source, destination), refusal);
        copy = answer == NULL ? NULL : cJSON_DetachItemFromObjectCaseSensitive(answer, "copy");
        cJSON_Delete(answer);
        runtime_remove_tree(folder);
    } else if (destination != NULL) {
        const char *parts[2] = {"a folder to fetch into could not be made at ", folder};
        *refusal = host_refusal_of(parts, 2, "check that the cache place can be written");
    }
    free(destination);
    free(folder);
    return copy;
}

static cJSON *copy_pair(const cJSON *source, cJSON *copy) {
    cJSON *pair = cJSON_CreateObject();
    cJSON *identity = pair == NULL ? NULL : cJSON_AddObjectToObject(pair, "source");
    if (identity == NULL || cJSON_AddStringToObject(identity, "id", host_text(source, "id")) == NULL ||
        !cJSON_AddItemToObject(pair, "copy", copy)) {
        cJSON_Delete(pair);
        cJSON_Delete(copy);
        return NULL;
    }
    return pair;
}

/** Adds source's copy to copies, or prints why it is left out; 0 when memory ran out. */
static int adds_copy_of(const host_context *context, const cJSON *source, int index, cJSON *copies) {
    if (host_text(source, "provider") != NULL) {
        const char *parts[3] = {"it names the provider ", host_text(source, "provider"), ", which only a plugin can ask for a copy"};
        char *detail = runtime_cache_text(parts, 3);
        const char *subject_parts[2] = {"marketplace ", host_text(source, "id")};
        char *subject = runtime_cache_text(subject_parts, 2);
        print_note(subject, detail);
        free(subject);
        free(detail);
        return 1;
    }
    cJSON *refusal = NULL;
    cJSON *copy = fetched_copy(context, source, index, &refusal);
    if (copy == NULL && refusal != NULL) {
        print_source_refusal(host_text(source, "id"), refusal);
        cJSON_Delete(refusal);
        return 1;
    }
    cJSON *pair = copy == NULL ? NULL : copy_pair(source, copy);
    if (pair == NULL || !cJSON_AddItemToArray(copies, pair)) {
        cJSON_Delete(pair);
        return 0;
    }
    return 1;
}

cJSON *host_read_marketplace_copies(const host_context *context, cJSON **refusal) {
    cJSON *listed = listed_sources(context, refusal);
    cJSON *copies = listed == NULL ? NULL : cJSON_CreateArray();
    int complete = copies != NULL;
    int index = 0;
    if (listed != NULL) {
        print_skipped_entries(cJSON_GetObjectItemCaseSensitive(listed, "skipped"));
    }
    const cJSON *source;
    cJSON_ArrayForEach(source, cJSON_GetObjectItemCaseSensitive(listed, "sources")) {
        complete = complete && adds_copy_of(context, source, index++, copies);
    }
    cJSON_Delete(listed);
    if (!complete) {
        cJSON_Delete(copies);
        copies = NULL;
    }
    return copies;
}
