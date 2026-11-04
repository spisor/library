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
#include "catalog_time.h"
#include "directory_tree.h"
#include "fetch.h"
#include "paths.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"
#include "spisor/path.h"
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
/** forebay/host's INDEX_DEADLINE_MS: an index is small, so a request that takes longer is stuck. */
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
#if defined(_WIN32)
    static volatile LONG download_number = 0;
    return (unsigned long)InterlockedIncrement(&download_number);
#else
    static unsigned long download_number = 0;
    return __atomic_add_fetch(&download_number, 1, __ATOMIC_RELAXED);
#endif
}

/** Everything one check of one source works with. */
typedef struct check_order {
    runtime_catalog_source source;
    const char *cache;
    char now[RUNTIME_CATALOG_TIME_SIZE];
    double now_milliseconds;
    double interval_hours;
    char *location;
    char *origin;
    char *state_path;
    runtime_catalog_state previous;
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
    free(written);
    return result;
}

static cJSON *answer_changed(cJSON *catalog) {
    cJSON *result = answer_kind("changed");
    if (!runtime_catalog_attach(result, "catalog", catalog)) {
        cJSON_Delete(result);
        return NULL;
    }
    return result;
}

static cJSON *refused_index(const check_order *order, const runtime_catalog_refusal *refusal) {
    char *message = runtime_catalog_refusal_message(refusal);
    cJSON *result = message == NULL ? NULL
                                    : answer_failed(order->location, " served an index the host refuses: ", message);
    free(message);
    return result;
}

/** Reads index and, when it is a catalog, keeps it as the source's state, answering whether it changed. */
static cJSON *accept_index(const check_order *order, char *etag, char *index, size_t length) {
    runtime_catalog_refusal refusal;
    cJSON *catalog = runtime_catalog_read_text(index, length, order->origin, &refusal);
    if (catalog == NULL) {
        cJSON *result = refused_index(order, &refusal);
        runtime_catalog_free_refusal(&refusal);
        return result;
    }
    runtime_catalog_state next = {etag, (char *)order->now, index};
    if (!runtime_catalog_write_state(order->state_path, &next)) {
        cJSON_Delete(catalog);
        return answer_failed(order->location, " could not be checked: ", "its state under the cache could not be written");
    }
    if (order->previous.index != NULL && strcmp(order->previous.index, index) == 0) {
        cJSON_Delete(catalog);
        return answer_kind("unchanged");
    }
    return answer_changed(catalog);
}

static cJSON *check_local(const check_order *order) {
    size_t length;
    uint8_t *bytes = spisor_read_file(order->location, &length);
    if (bytes == NULL) {
        return answer_failed(order->location, " could not be checked: ", "the file could not be read");
    }
    cJSON *result = accept_index(order, NULL, (char *)bytes, length);
    free(bytes);
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
    char *path = malloc(size);
    if (path != NULL) {
        snprintf(path, size, "%s.download-%d-%lu", state_path, (int)current_process_id(), next_download_number());
    }
    return path;
}

static cJSON *fetch_answer(const char *url, const char *destination, const char *etag, int32_t *status) {
    char *requested = requested_url(url);
    cJSON *request = cJSON_CreateObject();
    int built = requested != NULL && request != NULL && cJSON_AddStringToObject(request, "url", requested) != NULL &&
                cJSON_AddStringToObject(request, "destination", destination) != NULL &&
                cJSON_AddNumberToObject(request, "timeoutSeconds", INDEX_TIMEOUT_SECONDS) != NULL &&
                (etag == NULL || cJSON_AddStringToObject(request, "etag", etag) != NULL);
    char *text = built ? cJSON_PrintUnformatted(request) : NULL;
    free(requested);
    cJSON_Delete(request);
    if (text == NULL) {
        *status = SPISOR_STATUS_OUT_OF_MEMORY;
        return NULL;
    }
    uint8_t *out = NULL;
    size_t out_len = 0;
    *status = runtime_fetch_get((const uint8_t *)text, strlen(text), &out, &out_len);
    cJSON_free(text);
    cJSON *answer = out == NULL ? NULL : spisor_parse_json(out, out_len);
    spisor_release(out);
    return answer;
}

static cJSON *settle_not_modified(const check_order *order) {
    if (order->previous.index == NULL) {
        return answer_failed(order->source.location, " answered 304 to a request that carried no etag", "");
    }
    runtime_catalog_state kept = {order->previous.etag, (char *)order->now, order->previous.index};
    if (!runtime_catalog_write_state(order->state_path, &kept)) {
        return answer_failed(order->location, " could not be checked: ", "its state under the cache could not be written");
    }
    return answer_kind("unchanged");
}

static cJSON *settle_stored(const check_order *order, const cJSON *answer, const char *download) {
    size_t length;
    uint8_t *bytes = spisor_read_file(download, &length);
    runtime_remove_file_entry(download);
    if (bytes == NULL) {
        return answer_failed(order->location, " could not be checked: ", "the downloaded index could not be read");
    }
    size_t skipped = length >= 3 && memcmp(bytes, BYTE_ORDER_MARK, 3) == 0 ? 3 : 0;
    const cJSON *etag = cJSON_GetObjectItemCaseSensitive(answer, "etag");
    cJSON *result = accept_index(order, cJSON_IsString(etag) ? etag->valuestring : NULL, (char *)bytes + skipped,
                           length - skipped);
    free(bytes);
    return result;
}

static cJSON *settle_fetch(const check_order *order, const cJSON *answer, int32_t status, const char *download) {
    const char *outcome = runtime_catalog_text_member(answer, "outcome");
    if (status == SPISOR_STATUS_SUCCESS && outcome != NULL && strcmp(outcome, "not-modified") == 0) {
        return settle_not_modified(order);
    }
    if (status == SPISOR_STATUS_SUCCESS && outcome != NULL && strcmp(outcome, "stored") == 0) {
        return settle_stored(order, answer, download);
    }
    const char *message = runtime_catalog_text_member(answer, "message");
    return answer_failed(order->location, " could not be checked: ",
                         message != NULL ? message : "the request ran out of memory");
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
    int32_t status;
    cJSON *answer = fetch_answer(order->location, download, etag, &status);
    cJSON *result = status == SPISOR_STATUS_OUT_OF_MEMORY ? NULL : settle_fetch(order, answer, status, download);
    cJSON_Delete(answer);
    free(download);
    return result;
}

static void free_order(check_order *order) {
    free(order->location);
    free(order->origin);
    free(order->state_path);
    runtime_catalog_free_state(&order->previous);
}

static int prepare_order(check_order *order) {
    order->location = runtime_catalog_location_of(&order->source);
    order->origin = order->location == NULL ? NULL : runtime_catalog_origin_of(&order->source, order->location);
    order->state_path = order->location == NULL ? NULL : runtime_catalog_state_path(order->cache, order->location);
    if (order->state_path == NULL) {
        return 0;
    }
    runtime_catalog_read_state(order->state_path, &order->previous);
    return 1;
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

static int32_t read_check_order(const cJSON *argument, check_order *order, uint8_t **out, size_t *out_len) {
    memset(order, 0, sizeof *order);
    char *problem = NULL;
    int usable = runtime_catalog_source_of(cJSON_GetObjectItemCaseSensitive(argument, "source"), &order->source, &problem);
    free(problem);
    if (!usable) {
        return runtime_catalog_refuse_argument("source", "{kind: \"local\", path} or {kind: \"https\", url}", out, out_len);
    }
    order->cache = runtime_catalog_text_member(argument, "cache");
    if (order->cache == NULL || !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, order->cache)) {
        return runtime_catalog_refuse_argument("cache", "the absolute path of the cache place", out, out_len);
    }
    const char *now = runtime_catalog_text_member(argument, "now");
    if (now == NULL || !runtime_catalog_parse_time(now, &order->now_milliseconds) ||
        !runtime_catalog_format_time(order->now_milliseconds, order->now)) {
        return runtime_catalog_refuse_argument("now", "an ISO 8601 time, as toISOString writes it", out, out_len);
    }
    const cJSON *interval = cJSON_GetObjectItemCaseSensitive(argument, "intervalHours");
    if (!cJSON_IsNumber(interval)) {
        return runtime_catalog_refuse_argument("intervalHours", "a number of hours", out, out_len);
    }
    order->interval_hours = interval->valuedouble;
    return SPISOR_STATUS_SUCCESS;
}

RUNTIME_EXPORT int32_t runtime_catalog_check(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    spisor_call call;
    int32_t status = spisor_parse_call(in, in_len, 0, &call, out, out_len);
    if (status != SPISOR_STATUS_SUCCESS) {
        return status;
    }
    check_order order;
    status = read_check_order(call.argument, &order, out, out_len);
    if (status == SPISOR_STATUS_SUCCESS) {
        status = runtime_catalog_answer(checked(&order), out, out_len);
        free_order(&order);
    }
    spisor_discard_call(&call);
    return status;
}

/** The source's catalog as it stands, with no request: a local index on disk, an https one as last kept. */
static cJSON *current_catalog(const runtime_catalog_source *source, const char *cache) {
    check_order order;
    memset(&order, 0, sizeof order);
    order.source = *source;
    order.cache = cache;
    cJSON *catalog = NULL;
    if (prepare_order(&order) && order.origin != NULL) {
        size_t length = 0;
        char *index = source->local ? (char *)spisor_read_file(order.location, &length) : order.previous.index;
        length = source->local ? length : (index == NULL ? 0 : strlen(index));
        runtime_catalog_refusal refusal;
        catalog = index == NULL ? NULL : runtime_catalog_read_text(index, length, order.origin, &refusal);
        if (index != NULL && catalog == NULL) {
            runtime_catalog_free_refusal(&refusal);
        }
        if (source->local) {
            free(index);
        }
    }
    free_order(&order);
    return catalog;
}

static cJSON *current(const runtime_catalog_source *sources, size_t count, const char *cache) {
    runtime_catalog_reading *readings = malloc((count + 1) * sizeof *readings);
    if (readings == NULL) {
        return NULL;
    }
    size_t read = 0;
    for (size_t index = 0; index < count; index++) {
        cJSON *catalog = current_catalog(&sources[index], cache);
        if (catalog != NULL) {
            readings[read].source = sources[index];
            readings[read++].catalog = catalog;
        }
    }
    cJSON *merged = runtime_catalog_merge_readings(readings, read, NULL);
    for (size_t index = 0; index < read; index++) {
        cJSON_Delete((cJSON *)readings[index].catalog);
    }
    free(readings);
    cJSON *result = cJSON_CreateObject();
    if (!runtime_catalog_attach(result, "catalog", merged)) {
        cJSON_Delete(result);
        return NULL;
    }
    return result;
}

RUNTIME_EXPORT int32_t runtime_catalog_current(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    spisor_call call;
    int32_t status = spisor_parse_call(in, in_len, 0, &call, out, out_len);
    if (status != SPISOR_STATUS_SUCCESS) {
        return status;
    }
    const cJSON *listed = cJSON_GetObjectItemCaseSensitive(call.argument, "sources");
    const char *cache = runtime_catalog_text_member(call.argument, "cache");
    runtime_catalog_source *sources = NULL;
    size_t count = 0;
    char *problem = NULL;
    if (!cJSON_IsArray(listed) || !runtime_catalog_sources_of(listed, &sources, &count, &problem)) {
        status = problem == NULL && cJSON_IsArray(listed)
                     ? runtime_catalog_answer(NULL, out, out_len)
                     : runtime_catalog_refuse_argument("sources", "an array of catalog sources", out, out_len);
    } else if (cache == NULL || !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, cache)) {
        status = runtime_catalog_refuse_argument("cache", "the absolute path of the cache place", out, out_len);
    } else {
        status = runtime_catalog_answer(current(sources, count, cache), out, out_len);
    }
    free(problem);
    free(sources);
    spisor_discard_call(&call);
    return status;
}
