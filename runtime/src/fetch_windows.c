#include "fetch_transport.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <windows.h>
#include <winhttp.h>

#ifdef SPISOR_FETCH_TEST_HOOKS
#include "fetch_hooks.h"
#endif

#define READ_CHUNK 65536
#define CONNECT_MILLISECONDS 15000u
#define MAXIMUM_REDIRECTS 5
#define HEADER_TEXT_LIMIT 2048

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

static wchar_t *to_wide(const char *text) {
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (length <= 0) {
        return NULL;
    }
    wchar_t *wide = malloc((size_t)length * sizeof(wchar_t));
    if (wide != NULL && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, length) <= 0) {
        free(wide);
        return NULL;
    }
    return wide;
}

static char *to_utf8(const wchar_t *wide) {
    int length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (length <= 0) {
        return NULL;
    }
    char *narrow = malloc((size_t)length);
    if (narrow != NULL && WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow, length, NULL, NULL) <= 0) {
        free(narrow);
        return NULL;
    }
    return narrow;
}

static wchar_t *copy_wide(const wchar_t *text, size_t length) {
    wchar_t *copy = malloc((length + 1) * sizeof(wchar_t));
    if (copy != NULL) {
        wmemcpy(copy, text, length);
        copy[length] = 0;
    }
    return copy;
}

static void free_parts(url_parts *parts) {
    free(parts->wide_url);
    free(parts->host);
    free(parts->path);
}

static size_t without_fragment(const wchar_t *path, size_t length) {
    for (size_t index = 0; index < length; index++) {
        if (path[index] == L'#') {
            return index;
        }
    }
    return length;
}

static int split_url(const runtime_fetch_request *request, url_parts *parts) {
    memset(parts, 0, sizeof *parts);
    parts->wide_url = to_wide(request->url);
    URL_COMPONENTS components;
    memset(&components, 0, sizeof components);
    components.dwStructSize = sizeof components;
    components.dwHostNameLength = (DWORD)-1;
    components.dwUrlPathLength = (DWORD)-1;
    components.dwExtraInfoLength = (DWORD)-1;
    if (parts->wide_url == NULL || !WinHttpCrackUrl(parts->wide_url, 0, 0, &components)) {
        return 0;
    }
    int allowed = components.nScheme == INTERNET_SCHEME_HTTPS ||
                  (request->allow_plain_http && components.nScheme == INTERNET_SCHEME_HTTP);
    size_t path_length = without_fragment(components.lpszUrlPath,
                                          (size_t)components.dwUrlPathLength + components.dwExtraInfoLength);
    parts->host = copy_wide(components.lpszHostName, components.dwHostNameLength);
    parts->path = path_length == 0 ? copy_wide(L"/", 1) : copy_wide(components.lpszUrlPath, path_length);
    parts->port = components.nPort;
    parts->secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    return allowed && parts->host != NULL && parts->path != NULL && components.dwHostNameLength > 0;
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

static int limit_redirects(HINTERNET request) {
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    DWORD maximum = MAXIMUM_REDIRECTS;
    return WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy) &&
           WinHttpSetOption(request, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &maximum, sizeof maximum);
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
    return handles->request != NULL && limit_redirects(handles->request);
}

static wchar_t *conditional_headers(const runtime_fetch_request *request) {
    size_t size = 64 + (request->etag == NULL ? 0 : strlen(request->etag)) +
                  (request->last_modified == NULL ? 0 : strlen(request->last_modified));
    char *headers = malloc(size);
    if (headers == NULL) {
        return NULL;
    }
    int length = 0;
    headers[0] = 0;
    if (request->etag != NULL) {
        length += snprintf(headers + length, size - (size_t)length, "If-None-Match: %s\r\n", request->etag);
    }
    if (request->last_modified != NULL) {
        snprintf(headers + length, size - (size_t)length, "If-Modified-Since: %s\r\n", request->last_modified);
    }
    wchar_t *wide = to_wide(headers);
    free(headers);
    return wide;
}

static int send_request(const runtime_fetch_request *request, HINTERNET handle) {
    wchar_t *headers = conditional_headers(request);
    if (headers == NULL) {
        return 0;
    }
    int sent = WinHttpSendRequest(handle, headers[0] == 0 ? WINHTTP_NO_ADDITIONAL_HEADERS : headers,
                                  headers[0] == 0 ? 0 : (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
               WinHttpReceiveResponse(handle, NULL);
    free(headers);
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
    free(text);
    return known;
}

static void refuse_error(const runtime_fetch_request *request, DWORD error, runtime_fetch_answer *answer) {
    if (error == ERROR_WINHTTP_TIMEOUT) {
        runtime_fetch_refuse(answer, RUNTIME_FETCH_NETWORK_FIX, "%s did not finish within %u ms", request->url,
                             request->deadline_milliseconds);
    } else if (error == ERROR_WINHTTP_REDIRECT_FAILED) {
        runtime_fetch_refuse(answer, "check that the catalog still publishes this file",
                             "%s redirected more than %d times, or off https", request->url, MAXIMUM_REDIRECTS);
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
    uint8_t *chunk = malloc(READ_CHUNK);
    DWORD read = 1;
    int failed = chunk == NULL;
    while (!failed && read > 0 && !is_past(reading->deadline)) {
        failed = !WinHttpReadData(handle, chunk, READ_CHUNK, &read);
        if (!failed && read > 0 && !runtime_fetch_sink_take(sink, chunk, read)) {
            free(chunk);
            answer->outcome = RUNTIME_FETCH_STOPPED_BY_SINK;
            return;
        }
    }
    DWORD error = failed ? GetLastError() : (read > 0 ? ERROR_WINHTTP_TIMEOUT : ERROR_SUCCESS);
    free(chunk);
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

void runtime_fetch_over_network(const runtime_fetch_request *request, runtime_fetch_sink *sink,
                                runtime_fetch_answer *answer) {
    ULONGLONG deadline = GetTickCount64() + request->deadline_milliseconds;
    url_parts parts;
    if (!split_url(request, &parts)) {
        free_parts(&parts);
        runtime_fetch_refuse(answer, "name the file by a complete https url", "%s is not a usable https url",
                             request->url);
        return;
    }
    winhttp_handles handles;
    if (!open_handles(request, &parts, &handles) || !send_request(request, handles.request)) {
        refuse_error(request, GetLastError(), answer);
    } else {
        take_response(request, handles.request, deadline, sink, answer);
    }
    close_handles(&handles);
    free_parts(&parts);
}
