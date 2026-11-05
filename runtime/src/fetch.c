#include "fetch.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "directory_tree.h"
#include "fetch_transport.h"
#include "file_stream.h"
#include "paths.h"
#include "sha256.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"
#include "spisor/path.h"
#ifdef SPISOR_FETCH_TEST_HOOKS
#include "fetch_hooks.h"
#endif

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#define current_process_id() _getpid()
#else
#include <unistd.h>
#define current_process_id() getpid()
#endif

/**
 * @implNote One overall deadline standing for a 15 second connect plus a 60 second read.
 */
#define DOWNLOAD_DEADLINE_MILLISECONDS 75000u
#define LONGEST_TIMEOUT_SECONDS 86400.0
#define LARGEST_EXACT_INTEGER 9007199254740991.0
#define CORRUPT_FILE_FIX "never use it: ask the catalog's publisher to correct the file or its entry"
#define UNWRITABLE_FIX "free the cached file or point SPISOR_CACHE at a writable directory, then try again"

static int plain_http_allowed = 0;
static unsigned deadline_override = 0;

#ifdef SPISOR_FETCH_TEST_HOOKS
void runtime_fetch_allow_plain_http(int allowed) {
    plain_http_allowed = allowed;
}

void runtime_fetch_use_deadline(unsigned milliseconds) {
    deadline_override = milliseconds;
}
#endif

/**
 * @implNote fetch is called from worker threads, so the partial name carries a per-call number as well as
 * the process id: two fetches of one destination in one process never write into one partial file.
 */
static unsigned long next_partial_number(void) {
#if defined(_WIN32)
    static volatile LONG partial_number = 0;
    return (unsigned long)InterlockedIncrement(&partial_number);
#else
    static unsigned long partial_number = 0;
    return __atomic_add_fetch(&partial_number, 1, __ATOMIC_RELAXED);
#endif
}

typedef struct fetch_order {
    const char *url;
    const char *destination;
    const char *sha256;
    const char *etag;
    const char *last_modified;
    int limited;
    uint64_t max_bytes;
    unsigned deadline_milliseconds;
} fetch_order;

typedef struct fetch_files {
    char *partial;
    FILE *file;
} fetch_files;

static const char *optional_text(const cJSON *argument, const char *member, int *usable) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(argument, member);
    if (item == NULL) {
        return NULL;
    }
    if (!cJSON_IsString(item)) {
        *usable = 0;
        return NULL;
    }
    return item->valuestring;
}

static int is_lowercase_sha256(const char *text) {
    if (strlen(text) != 64) {
        return 0;
    }
    for (const char *at = text; *at != 0; at++) {
        if (!((*at >= '0' && *at <= '9') || (*at >= 'a' && *at <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

static int is_header_value(const char *text) {
    for (const unsigned char *at = (const unsigned char *)text; *at != 0; at++) {
        if (*at < 0x20 || *at == 0x7f) {
            return 0;
        }
    }
    return 1;
}

static int32_t refuse_argument(const char *member, const char *shape, uint8_t **out, size_t *out_len) {
    char message[256];
    char fix[512];
    snprintf(message, sizeof message, "the argument carries no usable %s", member);
    snprintf(fix, sizeof fix, "send %s as %s", member, shape);
    return spisor_fail(message, fix, out, out_len);
}

static int read_max_bytes(const cJSON *argument, fetch_order *order) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(argument, "maxBytes");
    if (item == NULL) {
        return 1;
    }
    double value = cJSON_IsNumber(item) ? item->valuedouble : -1;
    if (value < 0 || value > LARGEST_EXACT_INTEGER || floor(value) != value) {
        return 0;
    }
    order->limited = 1;
    order->max_bytes = (uint64_t)value;
    return 1;
}

static int read_timeout(const cJSON *argument, fetch_order *order) {
    order->deadline_milliseconds = DOWNLOAD_DEADLINE_MILLISECONDS;
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(argument, "timeoutSeconds");
    if (item == NULL) {
        return 1;
    }
    double seconds = cJSON_IsNumber(item) ? item->valuedouble : 0;
    if (!(seconds > 0 && seconds <= LONGEST_TIMEOUT_SECONDS)) {
        return 0;
    }
    order->deadline_milliseconds = (unsigned)ceil(seconds * 1000);
    return 1;
}

static int32_t read_order(const cJSON *argument, fetch_order *order, uint8_t **out, size_t *out_len) {
    int usable = 1;
    memset(order, 0, sizeof *order);
    order->url = optional_text(argument, "url", &usable);
    if (order->url == NULL || order->url[0] == 0) {
        return refuse_argument("url", "a non-empty https: or file: url", out, out_len);
    }
    order->destination = optional_text(argument, "destination", &usable);
    if (order->destination == NULL || !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, order->destination)) {
        return refuse_argument("destination", "an absolute path inside the cache", out, out_len);
    }
    order->sha256 = optional_text(argument, "sha256", &usable);
    if (!usable || (order->sha256 != NULL && !is_lowercase_sha256(order->sha256))) {
        return refuse_argument("sha256", "64 lowercase hex digits, or leave it out", out, out_len);
    }
    order->etag = optional_text(argument, "etag", &usable);
    order->last_modified = optional_text(argument, "lastModified", &usable);
    if (!usable || (order->etag != NULL && !is_header_value(order->etag)) ||
        (order->last_modified != NULL && !is_header_value(order->last_modified))) {
        return refuse_argument("etag or lastModified", "the header value a previous answer carried, on one line",
                               out, out_len);
    }
    if (!read_max_bytes(argument, order)) {
        return refuse_argument("maxBytes", "a whole number of bytes, zero or more", out, out_len);
    }
    if (!read_timeout(argument, order)) {
        return refuse_argument("timeoutSeconds", "a number of seconds above zero and at most a day", out, out_len);
    }
    return SPISOR_STATUS_SUCCESS;
}

typedef enum url_scheme {
    SCHEME_FILE,
    SCHEME_NETWORK,
    SCHEME_REFUSED
} url_scheme;

static int scheme_is(const char *url, const char *scheme) {
    size_t length = strlen(scheme);
    for (size_t index = 0; index < length; index++) {
        if (url[index] == 0 || (url[index] | 0x20) != scheme[index]) {
            return 0;
        }
    }
    return 1;
}

static url_scheme scheme_of(const char *url) {
    if (scheme_is(url, "file:")) {
        return SCHEME_FILE;
    }
    if (scheme_is(url, "https:") || (plain_http_allowed && scheme_is(url, "http:"))) {
        return SCHEME_NETWORK;
    }
    return SCHEME_REFUSED;
}

static size_t scheme_length(const char *url) {
    size_t length = strcspn(url, ":/?# ");
    return url[length] == ':' && length > 0 ? length + 1 : 0;
}

static int32_t refuse_scheme(const char *url, uint8_t **out, size_t *out_len) {
    char message[2048];
    size_t length = scheme_length(url);
    snprintf(message, sizeof message, "%s uses the scheme %.*s, which the cache does not fetch", url,
             length == 0 ? 6 : (int)length, length == 0 ? "(none)" : url);
    return spisor_fail(message, "serve the catalog's files over https or from a local directory", out, out_len);
}

static char *parent_of(const char *path) {
    const char *last = strrchr(path, '/');
    if (RUNTIME_HOST_IS_WINDOWS) {
        const char *backslash = strrchr(path, '\\');
        last = backslash != NULL && (last == NULL || backslash > last) ? backslash : last;
    }
    size_t length = last == NULL ? 0 : (size_t)(last - path);
    char *parent = malloc(length + 1);
    if (parent != NULL) {
        memcpy(parent, path, length);
        parent[length] = 0;
    }
    return parent;
}

static int make_parent(const char *destination) {
    char *parent = parent_of(destination);
    int made = parent != NULL && (parent[0] == 0 || spisor_make_directories(parent));
    free(parent);
    return made;
}

static int open_partial(const char *destination, fetch_files *files) {
    size_t size = strlen(destination) + 48;
    files->partial = malloc(size);
    files->file = NULL;
    if (files->partial == NULL) {
        return 0;
    }
    snprintf(files->partial, size, "%s.partial-%d-%lu", destination, (int)current_process_id(),
             next_partial_number());
    if (!make_parent(destination)) {
        return 0;
    }
    files->file = runtime_open_file(files->partial, RUNTIME_FILE_WRITE);
    return files->file != NULL;
}

static void discard_partial(fetch_files *files) {
    if (files->file != NULL) {
        fclose(files->file);
    }
    if (files->partial != NULL) {
        runtime_remove_file_entry(files->partial);
    }
    free(files->partial);
}

static int32_t refuse_unwritable(const char *destination, uint8_t **out, size_t *out_len) {
    char message[2048];
    snprintf(message, sizeof message, "%s could not be written", destination);
    return spisor_fail(message, UNWRITABLE_FIX, out, out_len);
}

static int32_t refuse_answer(const fetch_order *order, const runtime_fetch_answer *answer,
                             const runtime_fetch_sink *sink, uint8_t **out, size_t *out_len) {
    char message[RUNTIME_FETCH_DETAIL_SIZE + 2048];
    switch (answer->outcome) {
    case RUNTIME_FETCH_STATUS:
        snprintf(message, sizeof message, "%s answered HTTP %ld", order->url, answer->status);
        return spisor_fail(message, "check that the catalog still publishes this file", out, out_len);
    case RUNTIME_FETCH_CUT_OFF:
        snprintf(message, sizeof message, "%s ended after %" PRIu64 " of the %" PRIu64 " bytes it declared",
                 order->url, sink->bytes, answer->declared);
        return spisor_fail(message, RUNTIME_FETCH_NETWORK_FIX, out, out_len);
    case RUNTIME_FETCH_STOPPED_BY_SINK:
        if (sink->unwritable) {
            return refuse_unwritable(order->destination, out, out_len);
        }
        snprintf(message, sizeof message, "%s served more than the %" PRIu64 " bytes maxBytes allows", order->url,
                 order->max_bytes);
        return spisor_fail(message, CORRUPT_FILE_FIX, out, out_len);
    default:
        return spisor_fail(answer->detail, answer->fix, out, out_len);
    }
}

static int32_t refuse_unasked_304(const char *url, uint8_t **out, size_t *out_len) {
    char message[2048];
    snprintf(message, sizeof message, "%s answered 304 to a request that carried no etag or lastModified", url);
    return spisor_fail(message, "send the etag or lastModified the last stored answer carried, or neither", out,
                       out_len);
}

static int32_t out_of_memory(uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    return SPISOR_STATUS_OUT_OF_MEMORY;
}

static int32_t answer_not_modified(uint8_t **out, size_t *out_len) {
    cJSON *result = cJSON_CreateObject();
    if (result == NULL || cJSON_AddStringToObject(result, "outcome", "not-modified") == NULL) {
        cJSON_Delete(result);
        return out_of_memory(out, out_len);
    }
    int32_t status = spisor_succeed(result, out, out_len);
    cJSON_Delete(result);
    return status;
}

static int add_optional_text(cJSON *result, const char *member, const char *value) {
    return value == NULL || cJSON_AddStringToObject(result, member, value) != NULL;
}

static int32_t answer_stored(const char *sha256, uint64_t bytes, const runtime_fetch_answer *answer, uint8_t **out,
                             size_t *out_len) {
    cJSON *result = cJSON_CreateObject();
    int built = result != NULL && cJSON_AddStringToObject(result, "outcome", "stored") != NULL &&
                cJSON_AddStringToObject(result, "sha256", sha256) != NULL &&
                cJSON_AddNumberToObject(result, "bytes", (double)bytes) != NULL &&
                add_optional_text(result, "etag", answer->etag) &&
                add_optional_text(result, "lastModified", answer->last_modified);
    if (!built) {
        cJSON_Delete(result);
        return out_of_memory(out, out_len);
    }
    int32_t status = spisor_succeed(result, out, out_len);
    cJSON_Delete(result);
    return status;
}

static int32_t refuse_hash(const fetch_order *order, const char *actual, uint8_t **out, size_t *out_len) {
    char message[2048];
    snprintf(message, sizeof message, "%s has SHA-256 %s, but the request expects %s", order->url, actual, order->sha256);
    return spisor_fail(message, CORRUPT_FILE_FIX, out, out_len);
}

/**
 * @implNote On Windows a rename onto a file another process has open fails, and when two hosts share one cache that file is the very one being stored, so a
 * destination already holding these bytes is accepted.
 */
static int move_into_place(fetch_files *files, const char *destination, const char *sha256) {
    if (runtime_move_file_over(files->partial, destination)) {
        return 1;
    }
    char present[RUNTIME_SHA256_HEX_SIZE];
    return spisor_sha256_file(destination, present) && strcmp(present, sha256) == 0;
}

static int32_t settle_received(const fetch_order *order, fetch_files *files, runtime_fetch_sink *sink,
                               const runtime_fetch_answer *answer, uint8_t **out, size_t *out_len) {
    char actual[RUNTIME_SHA256_HEX_SIZE];
    runtime_sha256_finish_hex(&sink->hash, actual);
    int closed = fclose(files->file) == 0;
    files->file = NULL;
    if (!closed) {
        discard_partial(files);
        return refuse_unwritable(order->destination, out, out_len);
    }
    if (order->sha256 != NULL && strcmp(actual, order->sha256) != 0) {
        discard_partial(files);
        return refuse_hash(order, actual, out, out_len);
    }
    if (!move_into_place(files, order->destination, actual)) {
        discard_partial(files);
        return refuse_unwritable(order->destination, out, out_len);
    }
    discard_partial(files);
    return answer_stored(actual, sink->bytes, answer, out, out_len);
}

static void transfer(const fetch_order *order, url_scheme scheme, runtime_fetch_sink *sink,
                     runtime_fetch_answer *answer) {
    if (scheme == SCHEME_FILE) {
        runtime_fetch_file_url(order->url, sink, answer);
        return;
    }
    runtime_fetch_request request = {order->url, order->etag, order->last_modified,
                                     deadline_override != 0 ? deadline_override : order->deadline_milliseconds,
                                     plain_http_allowed};
    runtime_fetch_over_network(&request, sink, answer);
}

static int32_t settle(const fetch_order *order, fetch_files *files, runtime_fetch_sink *sink,
                      const runtime_fetch_answer *answer, uint8_t **out, size_t *out_len) {
    if (answer->outcome == RUNTIME_FETCH_RECEIVED) {
        return settle_received(order, files, sink, answer, out, out_len);
    }
    discard_partial(files);
    if (answer->outcome != RUNTIME_FETCH_NOT_MODIFIED) {
        return refuse_answer(order, answer, sink, out, out_len);
    }
    if (order->etag == NULL && order->last_modified == NULL) {
        return refuse_unasked_304(order->url, out, out_len);
    }
    return answer_not_modified(out, out_len);
}

static int32_t fetch(const fetch_order *order, uint8_t **out, size_t *out_len) {
    url_scheme scheme = scheme_of(order->url);
    if (scheme == SCHEME_REFUSED) {
        return refuse_scheme(order->url, out, out_len);
    }
    fetch_files files;
    if (!open_partial(order->destination, &files)) {
        discard_partial(&files);
        return refuse_unwritable(order->destination, out, out_len);
    }
    runtime_fetch_sink sink;
    runtime_fetch_sink_start(&sink, files.file, order->limited, order->max_bytes);
    runtime_fetch_answer answer;
    memset(&answer, 0, sizeof answer);
    transfer(order, scheme, &sink, &answer);
    int32_t status = settle(order, &files, &sink, &answer, out, out_len);
    free(answer.etag);
    free(answer.last_modified);
    return status;
}

RUNTIME_EXPORT int32_t runtime_fetch_get(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    spisor_call call;
    int32_t status = spisor_parse_call(in, in_len, 0, &call, out, out_len);
    if (status != SPISOR_STATUS_SUCCESS) {
        return status;
    }
    fetch_order order;
    status = read_order(call.argument, &order, out, out_len);
    if (status == SPISOR_STATUS_SUCCESS) {
        status = fetch(&order, out, out_len);
    }
    spisor_discard_call(&call);
    return status;
}
