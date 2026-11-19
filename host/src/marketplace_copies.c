#include "marketplace_copies.h"
#include "allocation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache_refusal.h"
#include "directory_tree.h"
#include "host_definition.h"
#include "host_output.h"
#include "host_service.h"
#include "install_settings.h"
#include "spisor/json_value.h"
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
    spisor_memory_release(safe_detail);
    spisor_memory_release(safe_subject);
}

static void print_skipped_entries(const runtime_marketplace_sources_answer *sources) {
    for (size_t index = 0; index < sources->skipped_count; index++) {
        char subject[FETCH_FOLDER_SIZE];
        snprintf(subject, sizeof subject, "marketplaces[%d]", (int)sources->skipped[index].index);
        print_note(subject, sources->skipped[index].detail);
    }
}

static void print_source_refusal(const char *source_id, const cJSON *refusal) {
    char *safe_id = host_printable(source_id);
    host_write(1, "marketplace ");
    host_write(1, safe_id == NULL ? "" : safe_id);
    host_write(1, " could not be read, so its names are not searched: ");
    host_print_refusal_printably(refusal);
    spisor_memory_release(safe_id);
}

static void print_provider_note(const runtime_marketplace_source *source) {
    const char *parts[3] = {"it names the provider ", source->provider, ", which only a plugin can ask for a copy"};
    char *detail = runtime_cache_text(parts, 3);
    const char *subject_parts[2] = {"marketplace ", source->id};
    char *subject = runtime_cache_text(subject_parts, 2);
    print_note(subject, detail);
    spisor_memory_release(subject);
    spisor_memory_release(detail);
}

/** Lists the sources the merged settings name into `sources`; 0 with *refusal set, NULL when memory ran out. */
static int list_sources(const host_context *context, runtime_marketplace_sources_answer *sources, cJSON **refusal) {
    const runtime_marketplace_definition *marketplace = runtime_marketplace_definition_of();
    runtime_install_settings settings = {NULL, context->portable, context->state};
    runtime_marketplace_sources_argument argument = SPISOR_INIT(runtime_marketplace_sources_argument);
    spisor_value_tree tree = SPISOR_INIT(spisor_value_tree);
    spisor_refusal declined = SPISOR_INIT(spisor_refusal);
    cJSON *values = runtime_install_settings_values(&settings, 0, 1, refusal);
    spisor_status status = values == NULL ? SPISOR_OUT_OF_MEMORY : spisor_value_of_json(values, &tree);
    argument.settings = &tree.root;
    if (status == SPISOR_OK) {
        status = marketplace->sources(marketplace->context, &argument, sources, &declined);
        *refusal = status == SPISOR_OK ? NULL : host_refusal_json(&declined);
    }
    marketplace->release_refusal(marketplace->context, &declined);
    spisor_value_release(&tree);
    cJSON_Delete(values);
    return status == SPISOR_OK;
}

static char *fetch_folder(const host_context *context, size_t index) {
    char name[FETCH_FOLDER_SIZE];
    snprintf(name, sizeof name, "marketplace-fetch-%ld-%d", (long)host_process_id(), (int)index);
    const char *segments[2] = {context->cache, name};
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
}

static int read_into(const runtime_marketplace_source *source, const char *destination,
                     runtime_marketplace_copy_answer *copy, cJSON **refusal) {
    const runtime_marketplace_definition *marketplace = runtime_marketplace_definition_of();
    runtime_marketplace_read_argument argument = SPISOR_INIT(runtime_marketplace_read_argument);
    spisor_refusal declined = SPISOR_INIT(spisor_refusal);
    argument.id = source->id;
    argument.url = source->url;
    argument.destination = destination;
    spisor_status status = marketplace->read(marketplace->context, &argument, copy, &declined);
    *refusal = status == SPISOR_OK ? NULL : host_refusal_json(&declined);
    marketplace->release_refusal(marketplace->context, &declined);
    return status == SPISOR_OK;
}

/** Reads the copy source serves into `copy`, fetched into a folder made and removed here; 0 with *refusal set. */
static int fetch_copy(const host_context *context, const runtime_marketplace_source *source, size_t index,
                      runtime_marketplace_copy_answer *copy, cJSON **refusal) {
    char *folder = fetch_folder(context, index);
    const char *segments[2] = {folder, COPY_FILE};
    char *destination = folder == NULL ? NULL : spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
    int fetched = 0;
    if (destination != NULL && runtime_remove_tree(folder) == SPISOR_OK &&
        spisor_make_directories(folder) == SPISOR_OK) {
        fetched = read_into(source, destination, copy, refusal);
        runtime_remove_tree(folder);
    } else if (destination != NULL) {
        const char *parts[2] = {"a folder to fetch into could not be made at ", folder};
        *refusal = host_refusal_of(parts, 2, "check that the cache place can be written");
    }
    spisor_memory_release(destination);
    spisor_memory_release(folder);
    return fetched;
}

/** Adds the copy of the source at index to copies, or prints why it is left out; 0 when memory ran out. */
static int adds_copy_of(const host_context *context, size_t index, host_marketplace_copies *copies) {
    const runtime_marketplace_source *source = &copies->sources.sources[index];
    if (source->kind != RUNTIME_MARKETPLACE_SOURCE_URL) {
        print_provider_note(source);
        return 1;
    }
    cJSON *refusal = NULL;
    runtime_marketplace_copy_answer *copy = &copies->answers[copies->count];
    if (fetch_copy(context, source, index, copy, &refusal)) {
        copies->copies[copies->count].source = source->id;
        copies->copies[copies->count].copy = copy->copy;
        copies->count++;
        return 1;
    }
    if (refusal == NULL) {
        return 0;
    }
    print_source_refusal(source->id, refusal);
    cJSON_Delete(refusal);
    return 1;
}

/** Room for one copy per listed source, each answer empty; 0 when memory runs out. */
static int make_room(host_marketplace_copies *copies) {
    runtime_marketplace_copy_answer empty = SPISOR_INIT(runtime_marketplace_copy_answer);
    size_t capacity = copies->sources.source_count == 0 ? 1 : copies->sources.source_count;
    copies->copies = spisor_memory_allocate(capacity * sizeof *copies->copies);
    copies->answers = spisor_memory_allocate(capacity * sizeof *copies->answers);
    for (size_t index = 0; copies->answers != NULL && index < capacity; index++) {
        copies->answers[index] = empty;
    }
    return copies->copies != NULL && copies->answers != NULL;
}

int host_read_marketplace_copies(const host_context *context, host_marketplace_copies *copies, cJSON **refusal) {
    runtime_marketplace_sources_answer empty = SPISOR_INIT(runtime_marketplace_sources_answer);
    memset(copies, 0, sizeof *copies);
    copies->sources = empty;
    *refusal = NULL;
    if (!list_sources(context, &copies->sources, refusal)) {
        return 0;
    }
    print_skipped_entries(&copies->sources);
    int complete = make_room(copies);
    for (size_t index = 0; complete && index < copies->sources.source_count; index++) {
        complete = adds_copy_of(context, index, copies);
    }
    return complete;
}

void host_release_marketplace_copies(host_marketplace_copies *copies) {
    const runtime_marketplace_definition *marketplace = runtime_marketplace_definition_of();
    for (size_t index = 0; copies->answers != NULL && index < copies->count; index++) {
        marketplace->release_copy_answer(marketplace->context, &copies->answers[index]);
    }
    marketplace->release_sources_answer(marketplace->context, &copies->sources);
    spisor_memory_release(copies->answers);
    spisor_memory_release(copies->copies);
    memset(copies, 0, sizeof *copies);
}
