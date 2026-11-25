#include "runtime/fetch.h"
#include "allocation.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "answer_storage.h"
#include "definition_support.h"
#include "directory_tree.h"
#include "prepared_undo.h"
#include "fetch_answer.h"
#include "fetch_file.h"
#include "file_stream.h"
#include "naula/memory.h"
#include "naula/transport.h"
#include "paths.h"
#include "sha256_file.h"
#include "spisor/file.h"
#include "spisor/path.h"
#include "atomic.h"

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
#define CORRUPT_FILE_FIX "never use it: ask the catalog's publisher to correct the file or its entry"
#define UNWRITABLE_FIX "free the cached file or point SPISOR_CACHE at a writable directory, then try again"

/**
 * @implNote fetch is called from worker threads, so the partial name carries a per-call number as well as
 * the process id: two fetches of one destination in one process never write into one partial file.
 */
static unsigned long next_partial_number(void) {
    static volatile long partial_number = 0;
    return (unsigned long)runtime_atomic_increment(&partial_number);
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
    if (text == NULL) {
        return 1;
    }
    for (const unsigned char *at = (const unsigned char *)text; *at != 0; at++) {
        if (*at < 0x20 || *at == 0x7f) {
            return 0;
        }
    }
    return 1;
}

static spisor_status refuse_argument(const char *member, const char *shape, spisor_refusal *refusal) {
    char detail[256];
    char fix[512];
    snprintf(detail, sizeof detail, "the argument carries no usable %s", member);
    snprintf(fix, sizeof fix, "send %s as %s", member, shape);
    return spisor_refuse_as(refusal, SPISOR_MALFORMED, detail, fix);
}

static int read_timeout(double seconds, fetch_order *order) {
    order->deadline_milliseconds = DOWNLOAD_DEADLINE_MILLISECONDS;
    if (seconds == 0) {
        return 1;
    }
    if (!(seconds > 0 && seconds <= LONGEST_TIMEOUT_SECONDS)) {
        return 0;
    }
    order->deadline_milliseconds = (unsigned)ceil(seconds * 1000);
    return 1;
}

static spisor_status read_order(const runtime_fetch_get_argument *argument, fetch_order *order,
                                spisor_refusal *refusal) {
    memset(order, 0, sizeof *order);
    order->url = argument->url;
    if (order->url == NULL || order->url[0] == 0) {
        return refuse_argument("url", "a non-empty https: or file: url", refusal);
    }
    order->destination = argument->destination;
    if (order->destination == NULL || !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, order->destination)) {
        return refuse_argument("destination", "an absolute path inside the cache", refusal);
    }
    order->sha256 = argument->sha256;
    if (order->sha256 != NULL && !is_lowercase_sha256(order->sha256)) {
        return refuse_argument("sha256", "64 lowercase hex digits, or NULL", refusal);
    }
    order->etag = argument->etag;
    order->last_modified = argument->last_modified;
    if (!is_header_value(order->etag) || !is_header_value(order->last_modified)) {
        return refuse_argument("etag or last_modified", "the header value a previous answer carried, on one line",
                               refusal);
    }
    order->limited = argument->limited != 0;
    order->max_bytes = argument->max_bytes;
    if (!read_timeout(argument->timeout_seconds, order)) {
        return refuse_argument("timeout_seconds", "a number of seconds above zero and at most a day, or 0", refusal);
    }
    return SPISOR_OK;
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
    if (scheme_is(url, "https:") || (naula_fetch_plain_http_is_allowed() && scheme_is(url, "http:"))) {
        return SCHEME_NETWORK;
    }
    return SCHEME_REFUSED;
}

static size_t scheme_length(const char *url) {
    size_t length = strcspn(url, ":/?# ");
    return url[length] == ':' && length > 0 ? length + 1 : 0;
}

static spisor_status refuse_scheme(const char *url, spisor_refusal *refusal) {
    char message[2048];
    size_t length = scheme_length(url);
    snprintf(message, sizeof message, "%s uses the scheme %.*s, which the cache does not fetch", url,
             length == 0 ? 6 : (int)length, length == 0 ? "(none)" : url);
    return spisor_refuse_as(refusal, SPISOR_REFUSED, message,
                             "serve the catalog's files over https or from a local directory");
}

static char *parent_of(const char *path) {
    const char *last = strrchr(path, '/');
    if (RUNTIME_HOST_IS_WINDOWS) {
        const char *backslash = strrchr(path, '\\');
        last = backslash != NULL && (last == NULL || backslash > last) ? backslash : last;
    }
    size_t length = last == NULL ? 0 : (size_t)(last - path);
    char *parent = spisor_memory_allocate(length + 1);
    if (parent != NULL) {
        memcpy(parent, path, length);
        parent[length] = 0;
    }
    return parent;
}

static spisor_status make_parent(const char *destination) {
    char *parent = parent_of(destination);
    spisor_status made = parent == NULL      ? SPISOR_OUT_OF_MEMORY
                         : parent[0] == 0 ? SPISOR_OK
                                          : spisor_make_directories(parent);
    spisor_memory_release(parent);
    return made;
}

static spisor_status open_partial(const char *destination, fetch_files *files) {
    size_t size = strlen(destination) + 48;
    files->partial = spisor_memory_allocate(size);
    files->file = NULL;
    if (files->partial == NULL) {
        return SPISOR_OUT_OF_MEMORY;
    }
    snprintf(files->partial, size, "%s.partial-%d-%lu", destination, (int)current_process_id(),
             next_partial_number());
    spisor_status made = make_parent(destination);
    if (made != SPISOR_OK) {
        return made;
    }
    int out_of_memory;
    files->file = runtime_open_file(files->partial, RUNTIME_FILE_WRITE, &out_of_memory);
    return files->file != NULL ? SPISOR_OK : out_of_memory ? SPISOR_OUT_OF_MEMORY : SPISOR_REFUSED;
}

static void discard_partial(fetch_files *files) {
    if (files->file != NULL) {
        fclose(files->file);
    }
    if (files->partial != NULL) {
        spisor_remove_file_in_place(files->partial);
    }
    spisor_memory_release(files->partial);
}

static spisor_status refuse(spisor_refusal *refusal, const char *detail, const char *fix) {
    return spisor_refuse_as(refusal, SPISOR_REFUSED, detail, fix);
}

static spisor_status refuse_unwritable(const char *destination, spisor_refusal *refusal) {
    char message[2048];
    snprintf(message, sizeof message, "%s could not be written", destination);
    return refuse(refusal, message, UNWRITABLE_FIX);
}

static spisor_status refuse_answer(const fetch_order *order, const naula_fetch_answer *answer,
                                   const naula_fetch_sink *sink, spisor_refusal *refusal) {
    char message[NAULA_FETCH_DETAIL_SIZE + 2048];
    switch (answer->outcome) {
    case NAULA_FETCH_STATUS:
        snprintf(message, sizeof message, "%s answered HTTP %ld", order->url, answer->status);
        return refuse(refusal, message, "check that the catalog still publishes this file");
    case NAULA_FETCH_CUT_OFF:
        if (runtime_fetch_is_deadline_cut_off(answer)) {
            return refuse(refusal, answer->detail, answer->fix);
        }
        snprintf(message, sizeof message, "%s ended after %" PRIu64 " of the %" PRIu64 " bytes it declared",
                 order->url, sink->bytes, answer->declared);
        return refuse(refusal, message, NAULA_FETCH_NETWORK_FIX);
    case NAULA_FETCH_STOPPED_BY_SINK:
        if (sink->unwritable) {
            return refuse_unwritable(order->destination, refusal);
        }
        snprintf(message, sizeof message, "%s served more than the %" PRIu64 " bytes max_bytes allows", order->url,
                 order->max_bytes);
        return refuse(refusal, message, CORRUPT_FILE_FIX);
    default:
        return refuse(refusal, answer->detail, answer->fix);
    }
}

static spisor_status refuse_unasked_304(const char *url, spisor_refusal *refusal) {
    char message[2048];
    snprintf(message, sizeof message, "%s answered 304 to a request that carried no etag or last_modified", url);
    return refuse(refusal, message, "send the etag or last_modified the last stored answer carried, or neither");
}

static void release_get_answer_storage(runtime_fetch_get_answer *answer) {
    runtime_release_stored_answer(answer, SPISOR_SHAPE_THROUGH(runtime_fetch_get_answer, storage));
}

/** Copies the stored answer's texts, taken before the file is moved into place so running out writes nothing. */
static int take_stored_answer(const char *sha256, uint64_t bytes, const naula_fetch_answer *received,
                              runtime_fetch_get_answer *answer) {
    if (!runtime_storage_copy(&answer->storage, sha256, &answer->sha256) ||
        !runtime_storage_copy(&answer->storage, received->etag, &answer->etag) ||
        !runtime_storage_copy(&answer->storage, received->last_modified, &answer->last_modified)) {
        release_get_answer_storage(answer);
        return 0;
    }
    answer->bytes = bytes;
    return 1;
}

static spisor_status refuse_hash(const fetch_order *order, const char *actual, spisor_refusal *refusal) {
    char message[2048];
    snprintf(message, sizeof message, "%s has SHA-256 %s, but the request expects %s", order->url, actual, order->sha256);
    return refuse(refusal, message, CORRUPT_FILE_FIX);
}

/**
 * @implNote On Windows a rename onto a file another process has open fails, and when two hosts share one cache that file is the very one being stored, so a
 * destination already holding these bytes is accepted.
 */
static spisor_status move_into_place(fetch_files *files, const char *destination, const char *sha256) {
    spisor_status moved = runtime_move_file_over(files->partial, destination);
    if (moved != SPISOR_REFUSED) {
        return moved;
    }
    char present[NAULA_SHA256_HEX_SIZE];
    spisor_status hashed = spisor_sha256_file(destination, present);
    if (hashed != SPISOR_OK) {
        return hashed;
    }
    if (strcmp(present, sha256) != 0) {
        return SPISOR_REFUSED;
    }
    return runtime_remove_file_entry(files->partial) == RUNTIME_ENTRY_OUT_OF_MEMORY ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
}

static spisor_status settle_received(const fetch_order *order, fetch_files *files, naula_fetch_sink *sink,
                                     const naula_fetch_answer *received, runtime_fetch_get_answer *answer,
                                     spisor_refusal *refusal) {
    char actual[NAULA_SHA256_HEX_SIZE];
    naula_sha256_hex(&sink->hash, actual);
    int closed = fclose(files->file) == 0;
    files->file = NULL;
    if (!closed) {
        discard_partial(files);
        return refuse_unwritable(order->destination, refusal);
    }
    if (order->sha256 != NULL && strcmp(actual, order->sha256) != 0) {
        discard_partial(files);
        return refuse_hash(order, actual, refusal);
    }
    if (!take_stored_answer(actual, sink->bytes, received, answer)) {
        discard_partial(files);
        return SPISOR_OUT_OF_MEMORY;
    }
    spisor_status moved = move_into_place(files, order->destination, actual);
    if (moved != SPISOR_OK) {
        discard_partial(files);
        release_get_answer_storage(answer);
        return moved == SPISOR_OUT_OF_MEMORY ? moved : refuse_unwritable(order->destination, refusal);
    }
    spisor_memory_release(files->partial);
    answer->outcome = RUNTIME_FETCH_GET_STORED;
    return SPISOR_OK;
}

static void transfer(const fetch_order *order, url_scheme scheme, naula_fetch_sink *sink,
                     naula_fetch_answer *answer) {
    if (scheme == SCHEME_FILE) {
        runtime_fetch_file_url(order->url, sink, answer);
        return;
    }
    naula_fetch_request request = {sizeof request, order->url, order->etag, order->last_modified,
                                   order->deadline_milliseconds, NULL};
    naula_fetch_over_network(&request, sink, answer);
}

static spisor_status settle(const fetch_order *order, fetch_files *files, naula_fetch_sink *sink,
                            const naula_fetch_answer *received, runtime_fetch_get_answer *answer,
                            spisor_refusal *refusal) {
    if (received->outcome == NAULA_FETCH_RECEIVED) {
        return settle_received(order, files, sink, received, answer, refusal);
    }
    discard_partial(files);
    if (received->outcome == NAULA_FETCH_OUT_OF_MEMORY) {
        return SPISOR_OUT_OF_MEMORY;
    }
    if (received->outcome != NAULA_FETCH_NOT_MODIFIED) {
        return refuse_answer(order, received, sink, refusal);
    }
    if (order->etag == NULL && order->last_modified == NULL) {
        return refuse_unasked_304(order->url, refusal);
    }
    answer->outcome = RUNTIME_FETCH_GET_NOT_MODIFIED;
    return SPISOR_OK;
}

static spisor_status fetch(const fetch_order *order, runtime_fetch_get_answer *answer, spisor_refusal *refusal) {
    url_scheme scheme = scheme_of(order->url);
    if (scheme == SCHEME_REFUSED) {
        return refuse_scheme(order->url, refusal);
    }
    fetch_files files;
    spisor_status opened = open_partial(order->destination, &files);
    if (opened != SPISOR_OK) {
        discard_partial(&files);
        return opened == SPISOR_OUT_OF_MEMORY ? opened : refuse_unwritable(order->destination, refusal);
    }
    naula_fetch_sink sink;
    naula_fetch_sink_start(&sink, files.file, order->limited, order->max_bytes);
    naula_fetch_answer received;
    memset(&received, 0, sizeof received);
    received.size = sizeof received;
    transfer(order, scheme, &sink, &received);
    spisor_status status = settle(order, &files, &sink, &received, answer, refusal);
    naula_memory_release(received.etag);
    naula_memory_release(received.last_modified);
    return status;
}

#define ARGUMENT_SHAPE SPISOR_SHAPE_THROUGH(runtime_fetch_get_argument, timeout_seconds)
#define ANSWER_SHAPE SPISOR_SHAPE_THROUGH(runtime_fetch_get_answer, storage)

static spisor_status get(void *context, const runtime_fetch_get_argument *argument, runtime_fetch_get_answer *answer,
                         spisor_refusal *refusal) {
    fetch_order order;
    spisor_status status;
    (void)context;
    if (argument == NULL || !spisor_has_shape(argument->size, ARGUMENT_SHAPE) ||
        !SPISOR_IS_EMPTY_ANSWER(answer, ANSWER_SHAPE)) {
        return spisor_refuse_shape(refusal, "fetch's get");
    }
    status = read_order(argument, &order, refusal);
    return status == SPISOR_OK ? fetch(&order, answer, refusal) : status;
}

static void release_answer(void *context, runtime_fetch_get_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, ANSWER_SHAPE);
}

static const runtime_fetch_definition definition = {
    sizeof(runtime_fetch_definition), NULL, get, release_answer, spisor_release_refusal,
};

RUNTIME_DEFINITION_API const void *runtime_fetch_definition_of(void) {
    return &definition;
}
