#ifndef RUNTIME_FETCH_TRANSPORT_H
#define RUNTIME_FETCH_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "sha256.h"

#define RUNTIME_FETCH_NETWORK_FIX "check the network, then try again"
#define RUNTIME_FETCH_PUBLISHED_FIX "check that the catalog still publishes this file"
#define RUNTIME_FETCH_DETAIL_SIZE 1024

/** Where fetched bytes go: the partial file or a memory buffer, the running hash and the byte count, bounded by limit when limited. */
typedef struct runtime_fetch_sink {
    FILE *file;
    uint8_t *memory;
    runtime_sha256 hash;
    uint64_t bytes;
    uint64_t limit;
    int limited;
    int exceeded;
    int unwritable;
} runtime_fetch_sink;

void runtime_fetch_sink_start(runtime_fetch_sink *sink, FILE *file, int limited, uint64_t limit);

/** A sink that keeps at most capacity bytes in buffer instead of a file; more than that marks it exceeded. */
void runtime_fetch_sink_start_memory(runtime_fetch_sink *sink, uint8_t *buffer, size_t capacity);

/** Writes and hashes the bytes. 0, and nothing more taken, once they would pass the limit or cannot be written. */
int runtime_fetch_sink_take(runtime_fetch_sink *sink, const uint8_t *bytes, size_t length);

/** Whether a body of the declared length fits the limit; one that does not marks the sink exceeded. */
int runtime_fetch_sink_admits(runtime_fetch_sink *sink, uint64_t declared);

typedef struct runtime_fetch_request {
    const char *url;
    const char *etag;
    const char *last_modified;
    unsigned deadline_milliseconds;
    int allow_plain_http;
    const char *authorization;
} runtime_fetch_request;

typedef enum runtime_fetch_outcome {
    RUNTIME_FETCH_RECEIVED,
    RUNTIME_FETCH_NOT_MODIFIED,
    RUNTIME_FETCH_STATUS,
    RUNTIME_FETCH_CUT_OFF,
    RUNTIME_FETCH_STOPPED_BY_SINK,
    RUNTIME_FETCH_REFUSED
} runtime_fetch_outcome;

/**
 * What a transport found. STATUS carries the status, CUT_OFF the declared length, and REFUSED its own
 * detail and fix; RECEIVED and NOT_MODIFIED may carry the validators the answer sent, freed with free.
 */
typedef struct runtime_fetch_answer {
    runtime_fetch_outcome outcome;
    long status;
    uint64_t declared;
    char *etag;
    char *last_modified;
    char detail[RUNTIME_FETCH_DETAIL_SIZE];
    const char *fix;
} runtime_fetch_answer;

void runtime_fetch_refuse(runtime_fetch_answer *answer, const char *fix, const char *format, ...);

/**
 * Fetches url into a memory sink under the same scheme rules and deadline override as fetch.get, so a
 * lookup that needs only a short answer shares its transport and refusals. authorization, when not NULL, is the
 * whole value of an Authorization header on one line.
 *
 * @implNote The header goes to the host and port url names and to no other: a redirect to another host is followed
 * without it. WinHTTP follows redirects by hand here, so the check is ours; libcurl drops a custom Authorization
 * header when a redirect changes host (7.58 and later, CURLOPT_UNRESTRICTED_AUTH left at 0).
 */
void runtime_fetch_into_memory(const char *url, const char *authorization, unsigned deadline_milliseconds,
                               runtime_fetch_sink *sink, runtime_fetch_answer *answer);

/** Whether the test hook has allowed plain http; always 0 in the shipped library. */
int runtime_fetch_plain_http_is_allowed(void);

/** Fetches an https url, or an http one when the request allows it, over the operating system's stack. */
void runtime_fetch_over_network(const runtime_fetch_request *request, runtime_fetch_sink *sink,
                                runtime_fetch_answer *answer);

/** Copies the file a file: url names into the sink. */
void runtime_fetch_file_url(const char *url, runtime_fetch_sink *sink, runtime_fetch_answer *answer);

#endif
