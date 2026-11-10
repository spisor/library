#ifndef RUNTIME_TEST_HTTP_FIXTURE_H
#define RUNTIME_TEST_HTTP_FIXTURE_H

#include <stddef.h>

#define HTTP_ROUTE_BODY_LENGTH (-1LL)
#define HTTP_ROUTE_UNTIL_CLOSE (-2LL)
#define HTTP_ROUTE_LIMITED_COUNT 128

/**
 * How the fixture answers a GET of path. declared_length is the Content-Length it sends:
 * HTTP_ROUTE_BODY_LENGTH sends the body's own, HTTP_ROUTE_UNTIL_CLOSE sends none and ends the body by
 * closing, and any larger number makes the answer end early, as a connection cut mid-body does. A
 * request whose If-None-Match equals etag, or whose If-Modified-Since equals last_modified, is answered
 * 304 with no body. A route with an answer_limit above zero answers that many requests and is then skipped, so a
 * later route with the same path answers next; at most HTTP_ROUTE_LIMITED_COUNT routes can be limited.
 */
typedef struct http_route {
    const char *path;
    int status;
    const char *extra_headers;
    const char *body;
    size_t body_length;
    long long declared_length;
    const char *etag;
    const char *last_modified;
    unsigned stall_milliseconds;
    unsigned answer_limit;
} http_route;

typedef struct http_fixture http_fixture;

/** A loopback HTTP/1.1 server on its own thread, one connection at a time, closing each after one answer. */
http_fixture *http_fixture_start(const http_route *routes, size_t count);

/**
 * A loopback proxy that records each request and refuses it with 502, so a test can see whether a client
 * sent a CONNECT for an https url and for which authority.
 */
http_fixture *http_fixture_start_proxy(void);

unsigned short http_fixture_port(const http_fixture *fixture);

/** Stops the thread and answers every request head it received, in order, freed with free. */
char *http_fixture_stop(http_fixture *fixture);

#endif
