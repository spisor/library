#include "definition_support.h"
#include "allocation.h"
#include "fetch_transport.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <windows.h>
#include <shlwapi.h>
#include <winhttp.h>

#ifdef SPISOR_FETCH_TEST_HOOKS
#include "fetch_hooks.h"
#endif

#define READ_CHUNK 65536
#define CONNECT_MILLISECONDS 15000u
#define MAXIMUM_REDIRECTS 5
#define HEADER_TEXT_LIMIT 2048
#define URL_LIMIT 4096

/**
 * @implNote The automatic access type (Windows 8.1 and later) takes the proxy the user's system settings
 * name, a PAC script or WPAD included, which is what lets a machine behind a corporate proxy fetch.
 */
static const DWORD session_access_type = WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;

#ifdef SPISOR_FETCH_TEST_HOOKS
unsigned long runtime_fetch_windows_access_type(void) {
    return session_access_type;
}
#endif

typedef struct winhttp_handles {
    HINTERNET session;
    HINTERNET connection;
    HINTERNET request;
} winhttp_handles;

typedef struct url_parts {
    wchar_t *wide_url;
    wchar_t *host;
    wchar_t *path;
    INTERNET_PORT port;
    int secure;
} url_parts;

/** text as UTF-16, freed; NULL when it is not UTF-8, or with *out_of_memory set to 1 when memory ran out. */
static wchar_t *to_wide_reporting(const char *text, int *out_of_memory) {
    *out_of_memory = 0;
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (length <= 0) {
        return NULL;
    }
    wchar_t *wide = spisor_memory_allocate((size_t)length * sizeof(wchar_t));
    *out_of_memory = wide == NULL;
    if (wide != NULL && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, length) <= 0) {
        spisor_memory_release(wide);
        return NULL;
    }
    return wide;
}

static char *to_utf8(const wchar_t *wide) {
    int length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (length <= 0) {
        return NULL;
    }
    char *narrow = spisor_memory_allocate((size_t)length);
    if (narrow != NULL && WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow, length, NULL, NULL) <= 0) {
        spisor_memory_release(narrow);
        return NULL;
    }
    return narrow;
}

static wchar_t *copy_wide(const wchar_t *text, size_t length) {
    wchar_t *copy = spisor_memory_allocate((length + 1) * sizeof(wchar_t));
    if (copy != NULL) {
        wmemcpy(copy, text, length);
        copy[length] = 0;
    }
    return copy;
}

static void free_parts(url_parts *parts) {
    spisor_memory_release(parts->wide_url);
    spisor_memory_release(parts->host);
    spisor_memory_release(parts->path);
}

static size_t without_fragment(const wchar_t *path, size_t length) {
    for (size_t index = 0; index < length; index++) {
        if (path[index] == L'#') {
            return index;
        }
    }
    return length;
}

/**
 * Splits wide_url, which parts then owns: SPISOR_MALFORMED when it is not an https url, or an http one plain http
 * allows, SPISOR_OUT_OF_MEMORY when memory runs out.
 */
static spisor_status split_url(int allow_plain_http, wchar_t *wide_url, url_parts *parts) {
    memset(parts, 0, sizeof *parts);
    parts->wide_url = wide_url;
    URL_COMPONENTS components;
    memset(&components, 0, sizeof components);
    components.dwStructSize = sizeof components;
    components.dwHostNameLength = (DWORD)-1;
    components.dwUrlPathLength = (DWORD)-1;
    components.dwExtraInfoLength = (DWORD)-1;
    if (parts->wide_url == NULL || !WinHttpCrackUrl(parts->wide_url, 0, 0, &components)) {
        return SPISOR_MALFORMED;
    }
    int allowed = components.nScheme == INTERNET_SCHEME_HTTPS ||
                  (allow_plain_http && components.nScheme == INTERNET_SCHEME_HTTP);
    if (!allowed || components.dwHostNameLength == 0) {
        return SPISOR_MALFORMED;
    }
    size_t path_length = without_fragment(components.lpszUrlPath,
                                          (size_t)components.dwUrlPathLength + components.dwExtraInfoLength);
    parts->host = copy_wide(components.lpszHostName, components.dwHostNameLength);
    parts->path = path_length == 0 ? copy_wide(L"/", 1) : copy_wide(components.lpszUrlPath, path_length);
    parts->port = components.nPort;
    parts->secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    return parts->host != NULL && parts->path != NULL ? SPISOR_OK : SPISOR_OUT_OF_MEMORY;
}

static void close_handles(winhttp_handles *handles) {
    if (handles->request != NULL) {
        WinHttpCloseHandle(handles->request);
    }
    if (handles->connection != NULL) {
        WinHttpCloseHandle(handles->connection);
    }
    if (handles->session != NULL) {
        WinHttpCloseHandle(handles->session);
    }
}

static int set_timeouts(HINTERNET session, unsigned deadline) {
    int connect = (int)(deadline < CONNECT_MILLISECONDS ? deadline : CONNECT_MILLISECONDS);
    return WinHttpSetTimeouts(session, (int)deadline, connect, (int)deadline, (int)deadline) != 0;
}

/**
 * @implNote Redirects are followed by hand because WinHTTP answers a redirect loop and a redirect it will not follow with the one error, and every hop must be checked to be
 * https before it is taken.
 */
static int disable_redirects(HINTERNET request) {
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    return WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy) != 0;
}

static int open_handles(const runtime_fetch_request *request, const url_parts *parts, winhttp_handles *handles) {
    memset(handles, 0, sizeof *handles);
    handles->session = WinHttpOpen(L"spisor-runtime", session_access_type, WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0);
    if (handles->session == NULL || !set_timeouts(handles->session, request->deadline_milliseconds)) {
        return 0;
    }
    handles->connection = WinHttpConnect(handles->session, parts->host, parts->port, 0);
    if (handles->connection == NULL) {
        return 0;
    }
    handles->request = WinHttpOpenRequest(handles->connection, L"GET", parts->path, NULL, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, parts->secure ? WINHTTP_FLAG_SECURE : 0);
    return handles->request != NULL && disable_redirects(handles->request);
}

static wchar_t *request_headers(const runtime_fetch_request *request, int is_authorized) {
    const char *authorization = is_authorized ? request->authorization : NULL;
    size_t size = 96 + (request->etag == NULL ? 0 : strlen(request->etag)) +
                  (request->last_modified == NULL ? 0 : strlen(request->last_modified)) +
                  (authorization == NULL ? 0 : strlen(authorization));
    char *headers = spisor_memory_allocate(size);
    if (headers == NULL) {
        return NULL;
    }
    int length = 0;
    headers[0] = 0;
    if (request->etag != NULL) {
        length += snprintf(headers + length, size - (size_t)length, "If-None-Match: %s\r\n", request->etag);
    }
    if (request->last_modified != NULL) {
        length += snprintf(headers + length, size - (size_t)length, "If-Modified-Since: %s\r\n", request->last_modified);
    }
    if (authorization != NULL) {
        snprintf(headers + length, size - (size_t)length, "Authorization: %s\r\n", authorization);
    }
    int out_of_memory;
    wchar_t *wide = to_wide_reporting(headers, &out_of_memory);
    runtime_overwrite(headers, size);
    spisor_memory_release(headers);
    return wide;
}

static int send_request(const runtime_fetch_request *request, HINTERNET handle, int is_authorized) {
    wchar_t *headers = request_headers(request, is_authorized);
    if (headers == NULL) {
        return 0;
    }
    int sent = WinHttpSendRequest(handle, headers[0] == 0 ? WINHTTP_NO_ADDITIONAL_HEADERS : headers,
                                  headers[0] == 0 ? 0 : (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
               WinHttpReceiveResponse(handle, NULL);
    runtime_overwrite(headers, wcslen(headers) * sizeof *headers);
    spisor_memory_release(headers);
    return sent;
}

static char *header_text(HINTERNET handle, DWORD query) {
    wchar_t value[HEADER_TEXT_LIMIT];
    DWORD size = sizeof value;
    if (!WinHttpQueryHeaders(handle, query, WINHTTP_HEADER_NAME_BY_INDEX, value, &size, WINHTTP_NO_HEADER_INDEX)) {
        return NULL;
    }
    return to_utf8(value);
}

static long status_of(HINTERNET handle) {
    DWORD status = 0;
    DWORD size = sizeof status;
    int known = WinHttpQueryHeaders(handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    return known ? (long)status : 0;
}

static int declared_length(HINTERNET handle, uint64_t *declared) {
    char *text = header_text(handle, WINHTTP_QUERY_CONTENT_LENGTH);
    int known = text != NULL && text[0] != 0 && strspn(text, "0123456789") == strlen(text) && strlen(text) < 20;
    if (known) {
        *declared = strtoull(text, NULL, 10);
    }
    spisor_memory_release(text);
    return known;
}

static void refuse_error(const runtime_fetch_request *request, DWORD error, runtime_fetch_answer *answer) {
    if (error == ERROR_WINHTTP_TIMEOUT) {
        runtime_fetch_refuse(answer, RUNTIME_FETCH_NETWORK_FIX, "%s did not finish within %u ms", request->url,
                             request->deadline_milliseconds);
    } else {
        runtime_fetch_refuse(answer, RUNTIME_FETCH_NETWORK_FIX, "%s could not be fetched: WinHTTP error %lu",
                             request->url, (unsigned long)error);
    }
}

static int is_past(ULONGLONG deadline) {
    return GetTickCount64() > deadline;
}

typedef struct body_reading {
    int declared_known;
    uint64_t declared;
    ULONGLONG deadline;
} body_reading;

static void read_body(const runtime_fetch_request *request, HINTERNET handle, const body_reading *reading,
                      runtime_fetch_sink *sink, runtime_fetch_answer *answer) {
    uint8_t *chunk = spisor_memory_allocate(READ_CHUNK);
    DWORD read = 1;
    int failed = chunk == NULL;
    while (!failed && read > 0 && !is_past(reading->deadline)) {
        failed = !WinHttpReadData(handle, chunk, READ_CHUNK, &read);
        if (!failed && read > 0 && !runtime_fetch_sink_take(sink, chunk, read)) {
            spisor_memory_release(chunk);
            answer->outcome = RUNTIME_FETCH_STOPPED_BY_SINK;
            return;
        }
    }
    DWORD error = failed ? GetLastError() : (read > 0 ? ERROR_WINHTTP_TIMEOUT : ERROR_SUCCESS);
    spisor_memory_release(chunk);
    if (reading->declared_known && sink->bytes < reading->declared && error != ERROR_WINHTTP_TIMEOUT) {
        answer->outcome = RUNTIME_FETCH_CUT_OFF;
        answer->declared = reading->declared;
    } else if (error != ERROR_SUCCESS) {
        refuse_error(request, error, answer);
    } else {
        answer->outcome = RUNTIME_FETCH_RECEIVED;
    }
}

static void take_response(const runtime_fetch_request *request, HINTERNET handle, ULONGLONG deadline,
                          runtime_fetch_sink *sink, runtime_fetch_answer *answer) {
    answer->status = status_of(handle);
    answer->etag = header_text(handle, WINHTTP_QUERY_ETAG);
    answer->last_modified = header_text(handle, WINHTTP_QUERY_LAST_MODIFIED);
    if (answer->status == 304) {
        answer->outcome = RUNTIME_FETCH_NOT_MODIFIED;
        return;
    }
    if (answer->status < 200 || answer->status > 299) {
        answer->outcome = RUNTIME_FETCH_STATUS;
        return;
    }
    body_reading reading = {0, 0, deadline};
    reading.declared_known = declared_length(handle, &reading.declared);
    if (reading.declared_known && !runtime_fetch_sink_admits(sink, reading.declared)) {
        answer->outcome = RUNTIME_FETCH_STOPPED_BY_SINK;
        return;
    }
    read_body(request, handle, &reading, sink, answer);
}

static int is_redirect(long status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

static wchar_t *redirect_target(HINTERNET handle, const wchar_t *current) {
    wchar_t location[URL_LIMIT];
    DWORD size = sizeof location;
    if (!WinHttpQueryHeaders(handle, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, location, &size,
                             WINHTTP_NO_HEADER_INDEX)) {
        return NULL;
    }
    wchar_t *target = spisor_memory_allocate(URL_LIMIT * sizeof(wchar_t));
    DWORD length = URL_LIMIT;
    if (target != NULL && UrlCombineW(current, location, target, &length, 0) != S_OK) {
        spisor_memory_release(target);
        return NULL;
    }
    return target;
}

typedef enum hop_outcome {
    HOP_ANSWERED,
    HOP_REDIRECTED
} hop_outcome;

static hop_outcome take_hop(const runtime_fetch_request *request, const url_parts *parts, int is_authorized,
                            ULONGLONG deadline, runtime_fetch_sink *sink, runtime_fetch_answer *answer,
                            wchar_t **next) {
    winhttp_handles handles;
    hop_outcome outcome = HOP_ANSWERED;
    if (!open_handles(request, parts, &handles) || !send_request(request, handles.request, is_authorized)) {
        refuse_error(request, GetLastError(), answer);
    } else if (is_redirect(status_of(handles.request))) {
        *next = redirect_target(handles.request, parts->wide_url);
        outcome = HOP_REDIRECTED;
    } else {
        take_response(request, handles.request, deadline, sink, answer);
    }
    close_handles(&handles);
    return outcome;
}

static void refuse_redirect(const runtime_fetch_request *request, runtime_fetch_answer *answer) {
    runtime_fetch_refuse(answer, RUNTIME_FETCH_PUBLISHED_FIX, "%s redirected to a url it may not follow", request->url);
}

typedef struct origin {
    wchar_t *host;
    INTERNET_PORT port;
} origin;

static void remember_origin(origin *known, const url_parts *parts) {
    known->host = copy_wide(parts->host, wcslen(parts->host));
    known->port = parts->port;
}

static int is_origin(const origin *known, const url_parts *parts) {
    return known->host != NULL && known->port == parts->port && _wcsicmp(known->host, parts->host) == 0;
}

static void take_hops(const runtime_fetch_request *request, origin *first, runtime_fetch_sink *sink,
                      runtime_fetch_answer *answer) {
    ULONGLONG deadline = GetTickCount64() + request->deadline_milliseconds;
    int out_of_memory;
    wchar_t *current = to_wide_reporting(request->url, &out_of_memory);
    if (out_of_memory) {
        runtime_fetch_out_of_memory(answer);
        return;
    }
    for (int hop = 0; hop <= MAXIMUM_REDIRECTS; hop++) {
        url_parts parts;
        spisor_status split = split_url(request->allow_plain_http, current, &parts);
        if (split == SPISOR_OUT_OF_MEMORY) {
            free_parts(&parts);
            runtime_fetch_out_of_memory(answer);
            return;
        }
        if (split != SPISOR_OK) {
            free_parts(&parts);
            if (hop == 0) {
                runtime_fetch_refuse(answer, "name the file by a complete https url", "%s is not a usable https url",
                                     request->url);
            } else {
                refuse_redirect(request, answer);
            }
            return;
        }
        current = NULL;
        if (hop == 0) {
            remember_origin(first, &parts);
        }
        hop_outcome outcome = take_hop(request, &parts, is_origin(first, &parts), deadline, sink, answer, &current);
        free_parts(&parts);
        if (outcome == HOP_ANSWERED) {
            return;
        }
        if (current == NULL) {
            refuse_redirect(request, answer);
            return;
        }
    }
    spisor_memory_release(current);
    runtime_fetch_refuse(answer, RUNTIME_FETCH_PUBLISHED_FIX, "%s redirected more than %d times", request->url,
                         MAXIMUM_REDIRECTS);
}

void runtime_fetch_over_network(const runtime_fetch_request *request, runtime_fetch_sink *sink,
                                runtime_fetch_answer *answer) {
    origin first = {NULL, 0};
    take_hops(request, &first, sink, answer);
    spisor_memory_release(first.host);
}
