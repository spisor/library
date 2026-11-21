#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog.h"
#include "catalog_answer.h"
#include "catalog_merge.h"
#include "catalog_read.h"
#include "catalog_source.h"
#include "catalog_state.h"
#include "catalog_text.h"
#include "allocation.h"
#include "catalog_time.h"
#include "prepared_undo.h"
#include "json_service.h"
#include "runtime/fetch.h"
#include "paths.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"
#include "spisor/path.h"
#include "atomic.h"
#ifdef SPISOR_CATALOG_TEST_HOOKS
#include "catalog_hooks.h"
#endif

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#define current_process_id() _getpid()
#else
#include <unistd.h>
#define current_process_id() getpid()
#endif

#define MILLISECONDS_PER_HOUR 3600000.0
/** An index is small, so a request that takes longer is stuck. */
#define INDEX_TIMEOUT_SECONDS 30
#define BYTE_ORDER_MARK "\xEF\xBB\xBF"

#ifdef SPISOR_CATALOG_TEST_HOOKS
static const char *redirected_from = NULL;
static const char *redirected_to = NULL;

void runtime_catalog_redirect_requests(const char *from, const char *to) {
    redirected_from = from;
    redirected_to = to;
}
#endif

static char *requested_url(const char *url) {
#ifdef SPISOR_CATALOG_TEST_HOOKS
    size_t prefix = redirected_from == NULL ? 0 : strlen(redirected_from);
    if (prefix > 0 && strncmp(url, redirected_from, prefix) == 0) {
        runtime_catalog_text text;
        runtime_catalog_text_start(&text);
        runtime_catalog_text_add(&text, redirected_to);
        runtime_catalog_text_add(&text, url + prefix);
        return runtime_catalog_text_finish(&text);
    }
#endif
    return runtime_catalog_copy(url);
}

static unsigned long next_download_number(void) {
    static volatile long download_number = 0;
    return (unsigned long)runtime_atomic_increment(&download_number);
}

/** Everything one check of one source works with. */
typedef struct check_order {
    runtime_catalog_location source;
    const char *cache;
    char now[RUNTIME_CATALOG_TIME_SIZE];
    double now_milliseconds;
    double interval_hours;
    char *location;
    char *origin;
    char *state_path;
    runtime_catalog_state previous;
    runtime_catalog_kept_state *kept;
} check_order;

static cJSON *answer_kind(const char *kind) {
    cJSON *result = cJSON_CreateObject();
    if (result == NULL || cJSON_AddStringToObject(result, "kind", kind) == NULL) {
        cJSON_Delete(result);
        return NULL;
    }
    return result;
}

static cJSON *answer_failed(const char *subject, const char *middle, const char *cause) {
    runtime_catalog_text detail;
    runtime_catalog_text_start(&detail);
    runtime_catalog_text_add(&detail, subject);
    runtime_catalog_text_add(&detail, middle);
    runtime_catalog_text_add(&detail, cause);
    char *written = runtime_catalog_text_finish(&detail);
    cJSON *result = written == NULL ? NULL : answer_kind("failed");
    if (result != NULL && cJSON_AddStringToObject(result, "detail", written) == NULL) {
        cJSON_Delete(result);
        result = NULL;
    }
    spisor_memory_release(written);
    return result;
}

static cJSON *answer_changed(cJSON *catalog) {
    cJSON *result = answer_kind("changed");
    if (!runtime_add_owned(result, "catalog", catalog)) {
        cJSON_Delete(result);
        return NULL;
    }
    return result;
}

static cJSON *refused_index(const check_order *order, const runtime_catalog_refusal *refusal) {
    char *message = runtime_catalog_refusal_message(refusal);
    cJSON *result = message == NULL ? NULL
                                    : answer_failed(order->location, " served an index the host refuses: ", message);
    spisor_memory_release(message);
    return result;
}

static char *copy_or_none(const char *text, int *out_of_memory) {
    char *copy = text == NULL ? NULL : runtime_catalog_copy(text);
    *out_of_memory = *out_of_memory || (text != NULL && copy == NULL);
    return copy;
}

/** Records the state the check keeps, for its caller to write once the answer stands: 0 when memory runs out. */
static int keep(const check_order *order, const char *etag, const char *index) {
    runtime_catalog_kept_state *kept = order->kept;
    int out_of_memory = 0;
    kept->path = copy_or_none(order->state_path, &out_of_memory);
    kept->location = copy_or_none(order->location, &out_of_memory);
    kept->state.etag = copy_or_none(etag, &out_of_memory);
    kept->state.checked_at = copy_or_none(order->now, &out_of_memory);
    kept->state.index = copy_or_none(index, &out_of_memory);
    if (out_of_memory) {
        runtime_catalog_free_kept_state(kept);
    }
    return !out_of_memory;
}

/** Reads index and, when it is a catalog, keeps it as the source's state, answering whether it changed. */
static cJSON *accept_index(const check_order *order, const char *etag, const char *index, size_t length) {
    runtime_catalog_refusal refusal;
    cJSON *catalog = NULL;
    spisor_status status = runtime_catalog_read_text(index, length, order->origin, &catalog, &refusal);
    if (status != SPISOR_OK) {
        cJSON *result = status == SPISOR_REFUSED ? refused_index(order, &refusal) : NULL;
        runtime_catalog_free_refusal(&refusal);
        return result;
    }
    cJSON *result;
    if (order->previous.index != NULL && strcmp(order->previous.index, index) == 0) {
        cJSON_Delete(catalog);
        result = answer_kind("unchanged");
    } else {
        result = answer_changed(catalog);
    }
    if (result != NULL && !keep(order, etag, index)) {
        cJSON_Delete(result);
        result = NULL;
    }
    return result;
}

/** The file at path, NUL-terminated, released with spisor_memory_release; NULL with *out_of_memory set or not. */
static uint8_t *read_index(const char *path, size_t *length, int *out_of_memory) {
    spisor_read_outcome outcome;
    uint8_t *bytes = spisor_read_file_reporting(path, length, &outcome);
    *out_of_memory = outcome == SPISOR_READ_OUT_OF_MEMORY;
    return bytes;
}

static cJSON *check_local(const check_order *order) {
    size_t length = 0;
    int out_of_memory;
    uint8_t *bytes = read_index(order->location, &length, &out_of_memory);
    if (bytes == NULL) {
        return out_of_memory ? NULL : answer_failed(order->location, " could not be checked: ", "the file could not be read");
    }
    cJSON *result = accept_index(order, NULL, (char *)bytes, length);
    spisor_memory_release(bytes);
    return result;
}

static int is_within_interval(const check_order *order) {
    double checked_at;
    if (order->previous.checked_at == NULL || !runtime_catalog_parse_time(order->previous.checked_at, &checked_at)) {
        return 0;
    }
    double elapsed = order->now_milliseconds - checked_at;
    return elapsed >= 0 && elapsed < order->interval_hours * MILLISECONDS_PER_HOUR;
}

static char *download_path(const char *state_path) {
    size_t size = strlen(state_path) + 48;
    char *path = spisor_memory_allocate(size);
    if (path != NULL) {
        snprintf(path, size, "%s.download-%d-%lu", state_path, (int)current_process_id(), next_download_number());
    }
    return path;
}

static spisor_status fetch_answer(const char *url, const char *destination, const char *etag,
                                  runtime_fetch_get_answer *answer, spisor_refusal *refusal) {
    const runtime_fetch_definition *fetch = runtime_fetch_definition_of();
    runtime_fetch_get_argument argument = SPISOR_INIT(runtime_fetch_get_argument);
    char *requested = requested_url(url);
    if (requested == NULL) {
        return SPISOR_OUT_OF_MEMORY;
    }
    argument.url = requested;
    argument.destination = destination;
    argument.etag = etag;
    argument.timeout_seconds = INDEX_TIMEOUT_SECONDS;
    spisor_status status = fetch->get(fetch->context, &argument, answer, refusal);
    spisor_memory_release(requested);
    return status;
}

static cJSON *settle_not_modified(const check_order *order) {
    if (order->previous.index == NULL) {
        return answer_failed(order->source.location, " answered 304 to a request that carried no etag", "");
    }
    cJSON *result = answer_kind("unchanged");
    if (result != NULL && !keep(order, order->previous.etag, order->previous.index)) {
        cJSON_Delete(result);
        result = NULL;
    }
    return result;
}

static cJSON *settle_stored(const check_order *order, const runtime_fetch_get_answer *answer, const char *download) {
    size_t length = 0;
    int out_of_memory;
    uint8_t *bytes = read_index(download, &length, &out_of_memory);
    spisor_remove_file_in_place(download);
    if (bytes == NULL) {
        return out_of_memory ? NULL
                             : answer_failed(order->location, " could not be checked: ",
                                             "the downloaded index could not be read");
    }
    size_t skipped = length >= 3 && memcmp(bytes, BYTE_ORDER_MARK, 3) == 0 ? 3 : 0;
    cJSON *result = accept_index(order, answer->etag, (char *)bytes + skipped, length - skipped);
    spisor_memory_release(bytes);
    return result;
}

static cJSON *settle_fetch(const check_order *order, spisor_status status, const runtime_fetch_get_answer *answer,
                           const spisor_refusal *refusal, const char *download) {
    if (status == SPISOR_OK && answer->outcome == RUNTIME_FETCH_GET_NOT_MODIFIED) {
        return settle_not_modified(order);
    }
    if (status == SPISOR_OK) {
        return settle_stored(order, answer, download);
    }
    return answer_failed(order->location, " could not be checked: ", refusal->detail);
}

static cJSON *check_https(const check_order *order) {
    if (strncmp(order->location, "https://", 8) != 0) {
        return answer_failed(order->location, " is not an https url; serve the catalog over https or name a local path",
                             "");
    }
    if (is_within_interval(order)) {
        return answer_kind("skipped");
    }
    char *download = download_path(order->state_path);
    if (download == NULL) {
        return NULL;
    }
    const char *etag = order->previous.index == NULL ? NULL : order->previous.etag;
    const runtime_fetch_definition *fetch = runtime_fetch_definition_of();
    runtime_fetch_get_answer answer = SPISOR_INIT(runtime_fetch_get_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status = fetch_answer(order->location, download, etag, &answer, &refusal);
    cJSON *result = status == SPISOR_OUT_OF_MEMORY ? NULL : settle_fetch(order, status, &answer, &refusal, download);
    fetch->release_answer(fetch->context, &answer);
    fetch->release_refusal(fetch->context, &refusal);
    spisor_memory_release(download);
    return result;
}

static void free_order(check_order *order) {
    spisor_memory_release(order->location);
    spisor_memory_release(order->origin);
    spisor_memory_release(order->state_path);
    runtime_catalog_free_state(&order->previous);
}

/** 1 with the order's places and previous state read, an origin NULL when the location has no url; 0 out of memory. */
static int prepare_order(check_order *order) {
    order->location = runtime_catalog_location_of(&order->source);
    spisor_status origin = order->location == NULL
                               ? SPISOR_OUT_OF_MEMORY
                               : runtime_catalog_origin_of(&order->source, order->location, &order->origin);
    if (origin == SPISOR_OUT_OF_MEMORY) {
        return 0;
    }
    order->state_path = runtime_catalog_state_path(order->cache, order->location);
    return order->state_path != NULL && runtime_catalog_read_state(order->state_path, &order->previous);
}

static cJSON *checked(check_order *order) {
    if (!prepare_order(order)) {
        return NULL;
    }
    if (order->origin == NULL) {
        return answer_failed(order->location, " could not be checked: ", "its path has no file url");
    }
    return order->source.local ? check_local(order) : check_https(order);
}

/** The order of one check: 1, or 0 with *refusal set, NULL when memory ran out. */
static int read_check_order(const runtime_catalog_check_request *request, check_order *order, cJSON **refusal) {
    memset(order, 0, sizeof *order);
    *refusal = NULL;
    char *problem = NULL;
    int usable = runtime_catalog_source_of(request->source, &order->source, &problem);
    int out_of_memory = !usable && problem == NULL;
    spisor_memory_release(problem);
    if (out_of_memory) {
        return 0;
    }
    if (!usable) {
        runtime_catalog_refuse_member("source", "{kind: \"local\", path} or {kind: \"https\", url}", refusal);
        return 0;
    }
    order->cache = request->cache;
    if (order->cache == NULL || !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, order->cache)) {
        runtime_catalog_refuse_member("cache", "the absolute path of the cache place", refusal);
        return 0;
    }
    const char *now = request->now;
    if (now == NULL || !runtime_catalog_parse_time(now, &order->now_milliseconds) ||
        !runtime_catalog_format_time(order->now_milliseconds, order->now)) {
        runtime_catalog_refuse_member("now", "an ISO 8601 time, as toISOString writes it", refusal);
        return 0;
    }
    order->interval_hours = request->interval_hours;
    return 1;
}

cJSON *runtime_catalog_check_deciding(const runtime_catalog_check_request *request, cJSON **refusal,
                                      runtime_catalog_kept_state *kept) {
    check_order order;
    cJSON *answer = NULL;
    memset(kept, 0, sizeof *kept);
    if (read_check_order(request, &order, refusal)) {
        order.kept = kept;
        answer = checked(&order);
        free_order(&order);
    }
    if (answer == NULL) {
        runtime_catalog_free_kept_state(kept);
    }
    return answer;
}

cJSON *runtime_catalog_unkept_answer(const runtime_catalog_kept_state *kept) {
    return answer_failed(kept->location, " could not be checked: ", "its state under the cache could not be written");
}

cJSON *runtime_catalog_check(const runtime_catalog_check_request *request, cJSON **refusal) {
    runtime_catalog_kept_state kept;
    cJSON *answer = runtime_catalog_check_deciding(request, refusal, &kept);
    spisor_status status = answer == NULL ? SPISOR_OK : runtime_catalog_keep_state(&kept);
    if (status != SPISOR_OK) {
        cJSON_Delete(answer);
        answer = status == SPISOR_REFUSED ? runtime_catalog_unkept_answer(&kept) : NULL;
    }
    runtime_catalog_free_kept_state(&kept);
    return answer;
}

/** The source's index as it stands: a local one on disk, an https one as last kept; NULL when there is none. */
static char *current_index(const check_order *order, size_t *length, int *out_of_memory) {
    if (order->source.local) {
        return (char *)read_index(order->location, length, out_of_memory);
    }
    *out_of_memory = 0;
    *length = order->previous.index == NULL ? 0 : strlen(order->previous.index);
    return order->previous.index;
}

/**
 * The source's catalog as it stands, with no request: OK with *catalog NULL when the source has none or it is
 * refused, or OUT_OF_MEMORY.
 */
static spisor_status current_catalog(const runtime_catalog_location *source, const char *cache, cJSON **catalog) {
    check_order order;
    memset(&order, 0, sizeof order);
    order.source = *source;
    order.cache = cache;
    *catalog = NULL;
    int prepared = prepare_order(&order);
    int out_of_memory = !prepared || order.location == NULL;
    if (!out_of_memory && order.origin != NULL) {
        size_t length = 0;
        char *index = current_index(&order, &length, &out_of_memory);
        runtime_catalog_refusal refusal;
        spisor_status status = index == NULL ? SPISOR_OK
                                             : runtime_catalog_read_text(index, length, order.origin, catalog, &refusal);
        out_of_memory = out_of_memory || status == SPISOR_OUT_OF_MEMORY;
        if (index != NULL) {
            runtime_catalog_free_refusal(&refusal);
        }
        if (source->local) {
            spisor_memory_release(index);
        }
    }
    free_order(&order);
    return out_of_memory ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
}

static cJSON *current(const runtime_catalog_location *sources, size_t count, const char *cache) {
    runtime_catalog_reading *readings = spisor_memory_allocate((count + 1) * sizeof *readings);
    if (readings == NULL) {
        return NULL;
    }
    size_t read = 0;
    spisor_status status = SPISOR_OK;
    for (size_t index = 0; status == SPISOR_OK && index < count; index++) {
        cJSON *catalog = NULL;
        status = current_catalog(&sources[index], cache, &catalog);
        if (catalog != NULL) {
            readings[read].source = sources[index];
            readings[read++].catalog = catalog;
        }
    }
    cJSON *merged = status == SPISOR_OK ? runtime_catalog_merge_readings(readings, read, NULL) : NULL;
    for (size_t index = 0; index < read; index++) {
        cJSON_Delete((cJSON *)readings[index].catalog);
    }
    spisor_memory_release(readings);
    return merged;
}

cJSON *runtime_catalog_current(const cJSON *listed, const char *cache, cJSON **refusal) {
    runtime_catalog_location *sources = NULL;
    size_t count = 0;
    char *problem = NULL;
    cJSON *answer = NULL;
    *refusal = NULL;
    if (!cJSON_IsArray(listed) || !runtime_catalog_sources_of(listed, &sources, &count, &problem)) {
        if (problem != NULL || !cJSON_IsArray(listed)) {
            runtime_catalog_refuse_member("sources", "an array of catalog sources", refusal);
        }
    } else if (cache == NULL || !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, cache)) {
        runtime_catalog_refuse_member("cache", "the absolute path of the cache place", refusal);
    } else {
        answer = current(sources, count, cache);
    }
    spisor_memory_release(problem);
    spisor_memory_release(sources);
    return answer;
}
