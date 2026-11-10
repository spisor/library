#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#endif

#include "http_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET fixture_socket;
#define INVALID_FIXTURE_SOCKET INVALID_SOCKET
#define close_socket closesocket
#define SEND_FLAGS 0
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int fixture_socket;
#define INVALID_FIXTURE_SOCKET (-1)
#define close_socket close
#define SEND_FLAGS 0
#endif

#define REQUEST_LIMIT 8192
#define LOG_LIMIT 65536

struct http_fixture {
    const http_route *routes;
    size_t route_count;
    unsigned served[HTTP_ROUTE_LIMITED_COUNT];
    int is_proxy;
    fixture_socket listener;
    unsigned short port;
    volatile int stopping;
    char log[LOG_LIMIT];
    size_t log_length;
#if defined(_WIN32)
    HANDLE thread;
#else
    pthread_t thread;
#endif
};

static void pause_milliseconds(unsigned milliseconds) {
#if defined(_WIN32)
    Sleep(milliseconds);
#else
    struct timespec duration = {(time_t)(milliseconds / 1000), (long)(milliseconds % 1000) * 1000000L};
    nanosleep(&duration, NULL);
#endif
}

static void send_all(fixture_socket connection, const char *bytes, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        int chunk = length - sent > 65536 ? 65536 : (int)(length - sent);
        int wrote = (int)send(connection, bytes + sent, chunk, SEND_FLAGS);
        if (wrote <= 0) {
            return;
        }
        sent += (size_t)wrote;
    }
}

static size_t read_request_head(fixture_socket connection, char *head, size_t capacity) {
    size_t length = 0;
    while (length + 1 < capacity) {
        int received = (int)recv(connection, head + length, (int)(capacity - 1 - length), 0);
        if (received <= 0) {
            break;
        }
        length += (size_t)received;
        head[length] = 0;
        if (strstr(head, "\r\n\r\n") != NULL) {
            break;
        }
    }
    head[length] = 0;
    return length;
}

static void record(http_fixture *fixture, const char *head, size_t length) {
    if (fixture->log_length + length + 1 < LOG_LIMIT) {
        memcpy(fixture->log + fixture->log_length, head, length);
        fixture->log_length += length;
        fixture->log[fixture->log_length] = 0;
    }
}

static int starts_with_ignoring_case(const char *text, const char *prefix) {
    for (; *prefix != 0; text++, prefix++) {
        char left = *text >= 'A' && *text <= 'Z' ? (char)(*text + 32) : *text;
        char right = *prefix >= 'A' && *prefix <= 'Z' ? (char)(*prefix + 32) : *prefix;
        if (left != right) {
            return 0;
        }
    }
    return 1;
}

static int header_equals(const char *head, const char *name, const char *expected) {
    if (expected == NULL) {
        return 0;
    }
    size_t name_length = strlen(name);
    size_t expected_length = strlen(expected);
    for (const char *line = strstr(head, "\r\n"); line != NULL; line = strstr(line + 2, "\r\n")) {
        const char *start = line + 2;
        if (starts_with_ignoring_case(start, name) && start[name_length] == ':') {
            const char *value = start + name_length + 1;
            while (*value == ' ') {
                value++;
            }
            return strncmp(value, expected, expected_length) == 0 && value[expected_length] == '\r';
        }
    }
    return 0;
}

static int is_exhausted(const http_fixture *fixture, size_t index) {
    return index < HTTP_ROUTE_LIMITED_COUNT && fixture->routes[index].answer_limit > 0 &&
           fixture->served[index] >= fixture->routes[index].answer_limit;
}

static const http_route *route_for(http_fixture *fixture, const char *head) {
    const char *path = strchr(head, ' ');
    if (path == NULL) {
        return NULL;
    }
    path++;
    for (size_t index = 0; index < fixture->route_count; index++) {
        size_t length = strlen(fixture->routes[index].path);
        if (strncmp(path, fixture->routes[index].path, length) == 0 && path[length] == ' ' &&
            !is_exhausted(fixture, index)) {
            if (index < HTTP_ROUTE_LIMITED_COUNT) {
                fixture->served[index]++;
            }
            return &fixture->routes[index];
        }
    }
    return NULL;
}

static void answer_not_found(fixture_socket connection) {
    const char *answer = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    send_all(connection, answer, strlen(answer));
}

static void answer_route(fixture_socket connection, const http_route *route, const char *head) {
    char status_line[4096];
    if (header_equals(head, "If-None-Match", route->etag) ||
        header_equals(head, "If-Modified-Since", route->last_modified)) {
        snprintf(status_line, sizeof status_line, "HTTP/1.1 304 Not Modified\r\nConnection: close\r\n\r\n");
        send_all(connection, status_line, strlen(status_line));
        return;
    }
    if (route->stall_milliseconds > 0) {
        pause_milliseconds(route->stall_milliseconds);
    }
    int length = snprintf(status_line, sizeof status_line, "HTTP/1.1 %d Fixture\r\nConnection: close\r\n%s",
                          route->status, route->extra_headers == NULL ? "" : route->extra_headers);
    if (route->etag != NULL) {
        length += snprintf(status_line + length, sizeof status_line - (size_t)length, "ETag: %s\r\n", route->etag);
    }
    if (route->last_modified != NULL) {
        length += snprintf(status_line + length, sizeof status_line - (size_t)length, "Last-Modified: %s\r\n",
                           route->last_modified);
    }
    if (route->declared_length != HTTP_ROUTE_UNTIL_CLOSE) {
        long long declared = route->declared_length == HTTP_ROUTE_BODY_LENGTH ? (long long)route->body_length
                                                                               : route->declared_length;
        length += snprintf(status_line + length, sizeof status_line - (size_t)length, "Content-Length: %lld\r\n",
                           declared);
    }
    snprintf(status_line + length, sizeof status_line - (size_t)length, "\r\n");
    send_all(connection, status_line, strlen(status_line));
    send_all(connection, route->body == NULL ? "" : route->body, route->body_length);
}

static void serve_connection(http_fixture *fixture, fixture_socket connection) {
    char head[REQUEST_LIMIT];
    size_t length = read_request_head(connection, head, sizeof head);
    if (length == 0) {
        return;
    }
    record(fixture, head, length);
    if (fixture->is_proxy) {
        const char *answer = "HTTP/1.1 502 Fixture Refuses\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(connection, answer, strlen(answer));
        return;
    }
    const http_route *route = route_for(fixture, head);
    if (route == NULL) {
        answer_not_found(connection);
    } else {
        answer_route(connection, route, head);
    }
}

static void serve(http_fixture *fixture) {
    while (!fixture->stopping) {
        fixture_socket connection = accept(fixture->listener, NULL, NULL);
        if (connection == INVALID_FIXTURE_SOCKET) {
            continue;
        }
        if (!fixture->stopping) {
            serve_connection(fixture, connection);
        }
#if defined(_WIN32)
        shutdown(connection, SD_SEND);
#else
        shutdown(connection, SHUT_WR);
#endif
        close_socket(connection);
    }
}

#if defined(_WIN32)
static DWORD WINAPI serving_entry(LPVOID argument) {
    serve(argument);
    return 0;
}
#else
static void *serving_entry(void *argument) {
    serve(argument);
    return NULL;
}
#endif

static int open_listener(http_fixture *fixture) {
    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    fixture->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fixture->listener == INVALID_FIXTURE_SOCKET) {
        return 0;
    }
    socklen_t size = sizeof address;
    if (bind(fixture->listener, (struct sockaddr *)&address, sizeof address) != 0 ||
        listen(fixture->listener, 16) != 0 ||
        getsockname(fixture->listener, (struct sockaddr *)&address, &size) != 0) {
        close_socket(fixture->listener);
        return 0;
    }
    fixture->port = ntohs(address.sin_port);
    return 1;
}

/** @implNote A client that gave up closes its end mid-answer, and macOS has no MSG_NOSIGNAL to stop that write raising SIGPIPE. */
static http_fixture *start(const http_route *routes, size_t count, int is_proxy) {
#if defined(_WIN32)
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return NULL;
    }
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    http_fixture *fixture = calloc(1, sizeof *fixture);
    if (fixture == NULL) {
        return NULL;
    }
    fixture->routes = routes;
    fixture->route_count = count;
    fixture->is_proxy = is_proxy;
    if (!open_listener(fixture)) {
        free(fixture);
        return NULL;
    }
#if defined(_WIN32)
    fixture->thread = CreateThread(NULL, 0, serving_entry, fixture, 0, NULL);
    int started = fixture->thread != NULL;
#else
    int started = pthread_create(&fixture->thread, NULL, serving_entry, fixture) == 0;
#endif
    if (!started) {
        close_socket(fixture->listener);
        free(fixture);
        return NULL;
    }
    return fixture;
}

http_fixture *http_fixture_start(const http_route *routes, size_t count) {
    return start(routes, count, 0);
}

http_fixture *http_fixture_start_proxy(void) {
    return start(NULL, 0, 1);
}

unsigned short http_fixture_port(const http_fixture *fixture) {
    return fixture->port;
}

static void wake_listener(const http_fixture *fixture) {
    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(fixture->port);
    fixture_socket waker = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (waker != INVALID_FIXTURE_SOCKET) {
        connect(waker, (struct sockaddr *)&address, sizeof address);
        close_socket(waker);
    }
}

char *http_fixture_stop(http_fixture *fixture) {
    fixture->stopping = 1;
    wake_listener(fixture);
#if defined(_WIN32)
    WaitForSingleObject(fixture->thread, INFINITE);
    CloseHandle(fixture->thread);
#else
    pthread_join(fixture->thread, NULL);
#endif
    close_socket(fixture->listener);
    char *log = malloc(fixture->log_length + 1);
    if (log != NULL) {
        memcpy(log, fixture->log, fixture->log_length + 1);
    }
    free(fixture);
#if defined(_WIN32)
    WSACleanup();
#endif
    return log;
}
