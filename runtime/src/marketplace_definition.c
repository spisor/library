#include "runtime/marketplace.h"

#include <stdio.h>

#include "answer_storage.h"
#include "definition_support.h"
#include "json_value.h"
#include "marketplace_build.h"
#include "marketplace_memory.h"
#include "marketplace_name_lookup.h"
#include "marketplace_read.h"
#include "marketplace_sources.h"
#include "paths.h"
#include "spisor/path.h"

#define BUILD_SHAPE RUNTIME_SHAPE_THROUGH(runtime_marketplace_build_argument, api_base)
#define SOURCES_SHAPE RUNTIME_SHAPE_THROUGH(runtime_marketplace_sources_argument, settings)
#define READ_SHAPE RUNTIME_SHAPE_THROUGH(runtime_marketplace_read_argument, destination)
#define LOOKUP_SHAPE RUNTIME_SHAPE_THROUGH(runtime_marketplace_lookup_argument, name)
#define REMEMBER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_marketplace_remember_argument, check_only)
#define BUILD_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_marketplace_build_answer, storage)
#define SOURCES_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_marketplace_sources_answer, storage)
#define COPY_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_marketplace_copy_answer, storage)
#define LOOKUP_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_marketplace_lookup_answer, storage)
#define REMEMBER_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_marketplace_remember_answer, storage)

/** The MALFORMED refusal for an argument lacking `what`, or holding it as a value that is no well-formed record. */
static spisor_status refuse_missing(spisor_refusal *refusal, const char *what) {
    char detail[160];
    snprintf(detail, sizeof detail, "the argument carries no usable %s", what);
    return runtime_refuse_as(refusal, SPISOR_MALFORMED, detail,
                             "send every required text, and each document as the record its schema describes");
}

static int add_text(cJSON *object, const char *name, const char *text) {
    return text == NULL || cJSON_AddStringToObject(object, name, text) != NULL;
}

/** Adds a record under name, leaving a NULL one out; MALFORMED for a value that is no well-formed record. */
static spisor_status add_record(cJSON *object, const char *name, const spisor_value *record) {
    cJSON *json = NULL;
    spisor_status status;
    if (record == NULL) {
        return SPISOR_OK;
    }
    status = record->kind == SPISOR_VALUE_RECORD ? spisor_json_of_value(record, &json) : SPISOR_MALFORMED;
    if (status == SPISOR_OK && !cJSON_AddItemToObject(object, name, json)) {
        cJSON_Delete(json);
        status = SPISOR_OUT_OF_MEMORY;
    }
    return status;
}

static const char *text_of(const cJSON *object, const char *name) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
}

/** The call's own status once built: MALFORMED refused naming what, OUT_OF_MEMORY with the call deleted. */
static spisor_status call_built(spisor_status status, cJSON **call, spisor_refusal *refusal, const char *what) {
    if (status == SPISOR_OK) {
        return SPISOR_OK;
    }
    cJSON_Delete(*call);
    *call = NULL;
    return status == SPISOR_MALFORMED ? refuse_missing(refusal, what) : status;
}

static spisor_status fill_build(const cJSON *answered, void *answer_space) {
    runtime_marketplace_build_answer *answer = answer_space;
    const cJSON *skipped = cJSON_GetObjectItemCaseSensitive(answered, "skipped");
    const cJSON *entry;
    runtime_marketplace_skipped_entry *entries;
    size_t index = 0;
    spisor_status status = runtime_storage_value(&answer->storage, cJSON_GetObjectItemCaseSensitive(answered, "copy"),
                                                 &answer->copy);
    if (status != SPISOR_OK || !runtime_storage_take_array(&answer->storage, (size_t)cJSON_GetArraySize(skipped),
                                                           sizeof *entries, (void **)&entries)) {
        return status == SPISOR_OK ? SPISOR_OUT_OF_MEMORY : status;
    }
    cJSON_ArrayForEach(entry, skipped) {
        if (!runtime_storage_copy(&answer->storage, text_of(entry, "id"), &entries[index].id) ||
            !runtime_storage_copy(&answer->storage, text_of(entry, "reason"), &entries[index].reason)) {
            return SPISOR_OUT_OF_MEMORY;
        }
        index++;
    }
    answer->skipped = entries;
    answer->skipped_count = index;
    return SPISOR_OK;
}

/** Builds from the list and previous copy `call` holds, which it deletes. */
static spisor_status serve_build(const runtime_marketplace_build_argument *argument, cJSON *call,
                                 runtime_marketplace_build_answer *answer, spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    runtime_marketplace_build_request request = {cJSON_GetObjectItemCaseSensitive(call, "list"),
                                                 argument->built,
                                                 argument->api_base,
                                                 argument->token,
                                                 argument->token_host,
                                                 cJSON_GetObjectItemCaseSensitive(call, "previous")};
    cJSON *answered = runtime_marketplace_build(&request, &operation_refusal);
    cJSON_Delete(call);
    return runtime_serve_answered(answered, operation_refusal, fill_build, answer, BUILD_ANSWER_SHAPE, refusal);
}

static spisor_status build(void *context, const runtime_marketplace_build_argument *argument,
                           runtime_marketplace_build_answer *answer, spisor_refusal *refusal) {
    cJSON *call;
    spisor_status status = SPISOR_OUT_OF_MEMORY;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, BUILD_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, BUILD_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "marketplace's build");
    }
    if (argument->list == NULL || argument->built == NULL) {
        return refuse_missing(refusal, "list or built");
    }
    if ((argument->token == NULL) != (argument->token_host == NULL)) {
        return runtime_refuse_as(refusal, SPISOR_MALFORMED, "the argument carries one of token and token_host alone",
                                 "send token_host, such as github.com, with the token, or neither");
    }
    call = cJSON_CreateObject();
    if (call != NULL) {
        status = add_record(call, "list", argument->list);
    }
    if (status == SPISOR_OK) {
        status = add_record(call, "previous", argument->previous);
    }
    status = call_built(status, &call, refusal, "list or previous copy");
    return status == SPISOR_OK ? serve_build(argument, call, answer, refusal) : status;
}

static spisor_status fill_sources(const cJSON *answered, void *answer_space) {
    runtime_marketplace_sources_answer *answer = answer_space;
    const cJSON *sources = cJSON_GetObjectItemCaseSensitive(answered, "sources");
    const cJSON *skipped = cJSON_GetObjectItemCaseSensitive(answered, "skipped");
    runtime_marketplace_source *filled;
    runtime_marketplace_skipped_source *left_out;
    const cJSON *entry;
    if (!runtime_storage_take_array(&answer->storage, (size_t)cJSON_GetArraySize(sources), sizeof *filled,
                                    (void **)&filled) ||
        !runtime_storage_take_array(&answer->storage, (size_t)cJSON_GetArraySize(skipped), sizeof *left_out,
                                    (void **)&left_out)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    cJSON_ArrayForEach(entry, sources) {
        runtime_marketplace_source *source = &filled[answer->source_count++];
        source->kind = text_of(entry, "url") != NULL ? RUNTIME_MARKETPLACE_SOURCE_URL
                                                      : RUNTIME_MARKETPLACE_SOURCE_PROVIDER;
        if (!runtime_storage_copy(&answer->storage, text_of(entry, "id"), &source->id) ||
            !runtime_storage_copy(&answer->storage, text_of(entry, "url"), &source->url) ||
            !runtime_storage_copy(&answer->storage, text_of(entry, "provider"), &source->provider)) {
            return SPISOR_OUT_OF_MEMORY;
        }
    }
    cJSON_ArrayForEach(entry, skipped) {
        runtime_marketplace_skipped_source *source = &left_out[answer->skipped_count++];
        const cJSON *index = cJSON_GetObjectItemCaseSensitive(entry, "index");
        source->index = cJSON_IsNumber(index) ? (size_t)index->valuedouble : 0;
        if (!runtime_storage_copy(&answer->storage, text_of(entry, "detail"), &source->detail)) {
            return SPISOR_OUT_OF_MEMORY;
        }
    }
    answer->sources = filled;
    answer->skipped = left_out;
    return SPISOR_OK;
}

/** Lists the sources of the settings `call` holds, which it deletes. */
static spisor_status serve_sources(cJSON *call, runtime_marketplace_sources_answer *answer, spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    cJSON *answered = runtime_marketplace_sources(cJSON_GetObjectItemCaseSensitive(call, "settings"),
                                                  &operation_refusal);
    cJSON_Delete(call);
    return runtime_serve_answered(answered, operation_refusal, fill_sources, answer, SOURCES_ANSWER_SHAPE, refusal);
}

static spisor_status sources(void *context, const runtime_marketplace_sources_argument *argument,
                             runtime_marketplace_sources_answer *answer, spisor_refusal *refusal) {
    cJSON *call;
    spisor_status status = SPISOR_OUT_OF_MEMORY;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, SOURCES_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, SOURCES_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "marketplace's sources");
    }
    if (argument->settings == NULL) {
        return refuse_missing(refusal, "settings");
    }
    call = cJSON_CreateObject();
    if (call != NULL) {
        status = add_record(call, "settings", argument->settings);
    }
    status = call_built(status, &call, refusal, "settings");
    return status == SPISOR_OK ? serve_sources(call, answer, refusal) : status;
}

static spisor_status fill_copy(const cJSON *answered, void *answer_space) {
    runtime_marketplace_copy_answer *answer = answer_space;
    return runtime_storage_value(&answer->storage, cJSON_GetObjectItemCaseSensitive(answered, "copy"), &answer->copy);
}

static spisor_status read_source(void *context, const runtime_marketplace_read_argument *argument,
                                 runtime_marketplace_copy_answer *answer, spisor_refusal *refusal) {
    runtime_marketplace_read_request request;
    cJSON *operation_refusal = NULL;
    cJSON *answered;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, READ_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, COPY_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "marketplace's read");
    }
    if (argument->id == NULL) {
        return refuse_missing(refusal, "source id");
    }
    if (argument->url == NULL) {
        char detail[256];
        snprintf(detail, sizeof detail, "the source %s has no url", argument->id);
        return runtime_refuse_as(refusal, SPISOR_MALFORMED, detail,
                                 "ask the plugin a provider source names for its copy; this library reads urls only");
    }
    if (argument->text == NULL &&
        (argument->destination == NULL || !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, argument->destination))) {
        return runtime_refuse_as(refusal, SPISOR_MALFORMED, "the argument carries neither text nor an absolute destination",
                                 "send text, the body already fetched, or destination, an absolute path to download into");
    }
    request = (runtime_marketplace_read_request){argument->id, argument->url, argument->text, argument->destination};
    answered = runtime_marketplace_read(&request, &operation_refusal);
    return runtime_serve_answered(answered, operation_refusal, fill_copy, answer, COPY_ANSWER_SHAPE, refusal);
}

static spisor_status fill_lookup(const cJSON *answered, void *answer_space) {
    runtime_marketplace_lookup_answer *answer = answer_space;
    const cJSON *ambiguous = cJSON_GetObjectItemCaseSensitive(answered, "ambiguous");
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answered, "notFound"))) {
        return SPISOR_NONE;
    }
    answer->match = ambiguous != NULL ? RUNTIME_MARKETPLACE_AMBIGUOUS : RUNTIME_MARKETPLACE_FOUND;
    if (!runtime_storage_copy(&answer->storage, text_of(answered, "id"), &answer->id) ||
        !runtime_storage_copy(&answer->storage, text_of(answered, "marketplace"), &answer->marketplace) ||
        !runtime_storage_texts(&answer->storage, ambiguous, &answer->ambiguous, &answer->ambiguous_count)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    return SPISOR_OK;
}

static spisor_status add_copies(cJSON *call, const runtime_marketplace_lookup_argument *argument) {
    cJSON *copies = cJSON_AddArrayToObject(call, "copies");
    spisor_status status = copies == NULL ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
    if (status == SPISOR_OK && argument->copy_count > 0 && argument->copies == NULL) {
        status = SPISOR_MALFORMED;
    }
    for (size_t index = 0; status == SPISOR_OK && index < argument->copy_count; index++) {
        cJSON *pair = cJSON_CreateObject();
        cJSON *source = pair == NULL ? NULL : cJSON_AddObjectToObject(pair, "source");
        if (argument->copies[index].source == NULL || argument->copies[index].copy == NULL) {
            status = SPISOR_MALFORMED;
        } else if (source == NULL || !add_text(source, "id", argument->copies[index].source)) {
            status = SPISOR_OUT_OF_MEMORY;
        } else {
            status = add_record(pair, "copy", argument->copies[index].copy);
        }
        if (status != SPISOR_OK || !cJSON_AddItemToArray(copies, pair)) {
            cJSON_Delete(pair);
            status = status == SPISOR_OK ? SPISOR_OUT_OF_MEMORY : status;
        }
    }
    return status;
}

/** Looks name up in the copies `call` holds, which it deletes. */
static spisor_status serve_lookup(cJSON *call, const char *name, runtime_marketplace_lookup_answer *answer,
                                  spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    cJSON *answered =
        runtime_marketplace_lookup(cJSON_GetObjectItemCaseSensitive(call, "copies"), name, &operation_refusal);
    cJSON_Delete(call);
    return runtime_serve_answered(answered, operation_refusal, fill_lookup, answer, LOOKUP_ANSWER_SHAPE, refusal);
}

static spisor_status lookup(void *context, const runtime_marketplace_lookup_argument *argument,
                            runtime_marketplace_lookup_answer *answer, spisor_refusal *refusal) {
    cJSON *call;
    spisor_status status = SPISOR_OUT_OF_MEMORY;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, LOOKUP_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, LOOKUP_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "marketplace's lookup");
    }
    if (argument->name == NULL) {
        return refuse_missing(refusal, "name");
    }
    call = cJSON_CreateObject();
    if (call != NULL) {
        status = add_copies(call, argument);
    }
    status = call_built(status, &call, refusal, "copies");
    return status == SPISOR_OK ? serve_lookup(call, argument->name, answer, refusal) : status;
}

static spisor_status fill_remember(const cJSON *answered, void *answer_space) {
    runtime_marketplace_remember_answer *answer = answer_space;
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answered, "needsUserAcceptance"))) {
        answer->memory = RUNTIME_MARKETPLACE_NEEDS_USER_ACCEPTANCE;
    } else {
        answer->memory = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answered, "settled"))
                             ? RUNTIME_MARKETPLACE_SETTLED
                             : RUNTIME_MARKETPLACE_REMEMBERED;
    }
    if (!runtime_storage_copy(&answer->storage, text_of(answered, "was"), &answer->was) ||
        !runtime_storage_copy(&answer->storage, text_of(answered, "now"), &answer->now)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    return SPISOR_OK;
}

/**
 * @implNote was and now are copied only for NEEDS_USER_ACCEPTANCE, which writes nothing, so a remember that wrote its
 * record allocates nothing after it.
 */
static spisor_status remember(void *context, const runtime_marketplace_remember_argument *argument,
                              runtime_marketplace_remember_answer *answer, spisor_refusal *refusal) {
    runtime_marketplace_remember_request request;
    cJSON *operation_refusal = NULL;
    cJSON *answered;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, REMEMBER_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, REMEMBER_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "marketplace's remember");
    }
    if (argument->state == NULL || argument->name == NULL || argument->id == NULL) {
        return refuse_missing(refusal, "state, name or id");
    }
    if (!runtime_is_usable_home(argument->state)) {
        return runtime_refuse_place(refusal, "state");
    }
    request = (runtime_marketplace_remember_request){argument->state, argument->marketplace, argument->name,
                                                     argument->id,    argument->accept,      argument->check_only == 0};
    answered = runtime_marketplace_remember(&request, &operation_refusal);
    return runtime_serve_answered(answered, operation_refusal, fill_remember, answer, REMEMBER_ANSWER_SHAPE, refusal);
}

static void release_build_answer(void *context, runtime_marketplace_build_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, BUILD_ANSWER_SHAPE);
}

static void release_sources_answer(void *context, runtime_marketplace_sources_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, SOURCES_ANSWER_SHAPE);
}

static void release_copy_answer(void *context, runtime_marketplace_copy_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, COPY_ANSWER_SHAPE);
}

static void release_lookup_answer(void *context, runtime_marketplace_lookup_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, LOOKUP_ANSWER_SHAPE);
}

static void release_remember_answer(void *context, runtime_marketplace_remember_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, REMEMBER_ANSWER_SHAPE);
}

static const runtime_marketplace_definition definition = {
    sizeof(runtime_marketplace_definition),
    NULL,
    build,
    sources,
    read_source,
    lookup,
    remember,
    release_build_answer,
    release_sources_answer,
    release_copy_answer,
    release_lookup_answer,
    release_remember_answer,
    runtime_release_refusal,
};

RUNTIME_DEFINITION_API const void *runtime_marketplace_definition_of(void) {
    return &definition;
}
