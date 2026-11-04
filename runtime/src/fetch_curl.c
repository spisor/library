#define _POSIX_C_SOURCE 200809L

#include "fetch_transport.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#ifdef SPISOR_FETCH_TEST_HOOKS
#include "fetch_hooks.h"
#endif

#define CONNECT_MILLISECONDS 15000L
#define MAXIMUM_REDIRECTS 5L
#define ERROR_BUFFER_SIZE 256

/**
 * @implNote The option, info and code numbers below are libcurl's public ABI from curl.h, unchanged since
 * 7.16 and kept by curl's compatibility promise, so nothing is needed at build time. CURLOPT_PROTOCOLS is
 * the bitmask form, deprecated in 7.85 in favour of a string form older libraries lack, and still honoured.
 */
enum {
    CURLOPT_WRITEDATA = 10001,
    CURLOPT_URL = 10002,
    CURLOPT_ERRORBUFFER = 10010,
    CURLOPT_USERAGENT = 10018,
    CURLOPT_HTTPHEADER = 10023,
    CURLOPT_HEADERDATA = 10029,
    CURLOPT_WRITEFUNCTION = 20011,
    CURLOPT_HEADERFUNCTION = 20079,
    CURLOPT_NOPROGRESS = 43,
    CURLOPT_FOLLOWLOCATION = 52,
    CURLOPT_MAXREDIRS = 68,
    CURLOPT_NOSIGNAL = 99,
    CURLOPT_TIMEOUT_MS = 155,
    CURLOPT_CONNECTTIMEOUT_MS = 156,
    CURLOPT_PROTOCOLS = 181,
    CURLOPT_REDIR_PROTOCOLS = 182
};
enum { CURLPROTO_HTTP = 1, CURLPROTO_HTTPS = 2 };
enum { CURLINFO_RESPONSE_CODE = 0x200002 };
enum {
    CURLE_OK = 0,
    CURLE_PARTIAL_FILE = 18,
    CURLE_WRITE_ERROR = 23,
    CURLE_OPERATION_TIMEDOUT = 28,
    CURLE_TOO_MANY_REDIRECTS = 47
};

typedef void curl_handle;
typedef struct curl_list curl_list;
typedef size_t (*curl_data_function)(char *, size_t, size_t, void *);

typedef struct curl_api {
    void *handle;
    curl_handle *(*easy_init)(void);
    int (*easy_setopt)(curl_handle *, int, ...);
    int (*easy_perform)(curl_handle *);
    int (*easy_getinfo)(curl_handle *, int, ...);
    void (*easy_cleanup)(curl_handle *);
    const char *(*easy_strerror)(int);
    curl_list *(*slist_append)(curl_list *, const char *);
    void (*slist_free_all)(curl_list *);
} curl_api;

#if defined(__APPLE__)
static const char *const default_library_names[] = {"/usr/lib/libcurl.4.dylib", NULL};
#define MISSING_LIBCURL_FIX "restore /usr/lib/libcurl.4.dylib, which macOS itself ships"
#else
static const char *const default_library_names[] = {"libcurl.so.4", "libcurl.so", NULL};
#define MISSING_LIBCURL_FIX "install the libcurl package (libcurl4 on Debian and Ubuntu)"
#endif

/**
 * libcurl, loaded on first use and kept for the life of the process.
 *
 * @implNote Every access goes through load_api, which holds api_lock, so any thread may fetch. An absent
 * library is remembered too, so a machine without it pays one failed dlopen.
 */
static curl_api loaded_api;
static int load_attempted;
static const char *library_name_override;
static pthread_mutex_t api_lock = PTHREAD_MUTEX_INITIALIZER;

#ifdef SPISOR_FETCH_TEST_HOOKS
void runtime_fetch_curl_use_library(const char *library_name) {
    pthread_mutex_lock(&api_lock);
    if (loaded_api.handle != NULL) {
        dlclose(loaded_api.handle);
    }
    memset(&loaded_api, 0, sizeof loaded_api);
    load_attempted = 0;
    library_name_override = library_name;
    pthread_mutex_unlock(&api_lock);
}
#endif

static int resolve(void *handle, const char *symbol, void *slot) {
    void *address = dlsym(handle, symbol);
    memcpy(slot, &address, sizeof address);
    return address != NULL;
}

static int resolve_all(curl_api *api) {
    void *handle = api->handle;
    return resolve(handle, "curl_easy_init", &api->easy_init) &&
           resolve(handle, "curl_easy_setopt", &api->easy_setopt) &&
           resolve(handle, "curl_easy_perform", &api->easy_perform) &&
           resolve(handle, "curl_easy_getinfo", &api->easy_getinfo) &&
           resolve(handle, "curl_easy_cleanup", &api->easy_cleanup) &&
           resolve(handle, "curl_easy_strerror", &api->easy_strerror) &&
           resolve(handle, "curl_slist_append", &api->slist_append) &&
           resolve(handle, "curl_slist_free_all", &api->slist_free_all);
}

static void *open_library(void) {
    if (library_name_override != NULL) {
        return dlopen(library_name_override, RTLD_NOW | RTLD_LOCAL);
    }
    void *handle = NULL;
    for (size_t index = 0; handle == NULL && default_library_names[index] != NULL; index++) {
        handle = dlopen(default_library_names[index], RTLD_NOW | RTLD_LOCAL);
    }
    return handle;
}

static const curl_api *load_api(void) {
    pthread_mutex_lock(&api_lock);
    if (!load_attempted) {
        load_attempted = 1;
        loaded_api.handle = open_library();
        if (loaded_api.handle != NULL && !resolve_all(&loaded_api)) {
            dlclose(loaded_api.handle);
            memset(&loaded_api, 0, sizeof loaded_api);
        }
    }
    const curl_api *api = loaded_api.handle != NULL ? &loaded_api : NULL;
    pthread_mutex_unlock(&api_lock);
    return api;
}

typedef struct transfer {
    runtime_fetch_sink *sink;
    runtime_fetch_answer *answer;
    long status;
    int declared_known;
    uint64_t declared;
    int refused_status;
} transfer;

static int is_success(long status) {
    return status >= 200 && status <= 299;
}

static int name_is(const char *line, size_t length, const char *name) {
    size_t name_length = strlen(name);
    if (length <= name_length || line[name_length] != ':') {
        return 0;
    }
    for (size_t index = 0; index < name_length; index++) {
        if ((line[index] | 0x20) != name[index]) {
            return 0;
        }
    }
    return 1;
}

static char *header_value(const char *line, size_t length, size_t name_length) {
    size_t start = name_length + 1;
    while (start < length && (line[start] == ' ' || line[start] == '\t')) {
        start++;
    }
    size_t end = length;
    while (end > start && (line[end - 1] == '\r' || line[end - 1] == '\n' || line[end - 1] == ' ')) {
        end--;
    }
    char *value = malloc(end - start + 1);
    if (value != NULL) {
        memcpy(value, line + start, end - start);
        value[end - start] = 0;
    }
    return value;
}

static void replace_text(char **slot, char *value) {
    free(*slot);
    *slot = value;
}

static void begin_response(transfer *state, const char *line) {
    const char *space = strchr(line, ' ');
    state->status = space == NULL ? 0 : strtol(space + 1, NULL, 10);
    state->declared_known = 0;
    replace_text(&state->answer->etag, NULL);
    replace_text(&state->answer->last_modified, NULL);
}

static void take_declared_length(transfer *state, const char *line, size_t length) {
    char *value = header_value(line, length, strlen("content-length"));
    int digits = value != NULL && value[0] != 0 && strspn(value, "0123456789") == strlen(value) && strlen(value) < 20;
    state->declared_known = digits;
    state->declared = digits ? strtoull(value, NULL, 10) : 0;
    free(value);
}

static int headers_end(transfer *state) {
    if (!is_success(state->status) || !state->declared_known) {
        return 1;
    }
    return runtime_fetch_sink_admits(state->sink, state->declared);
}

static size_t take_header(char *line, size_t size, size_t count, void *context) {
    transfer *state = context;
    size_t length = size * count;
    if (length >= 5 && strncmp(line, "HTTP/", 5) == 0) {
        begin_response(state, line);
    } else if (length <= 2 && (line[0] == '\r' || line[0] == '\n')) {
        return headers_end(state) ? length : 0;
    } else if (name_is(line, length, "etag")) {
        replace_text(&state->answer->etag, header_value(line, length, strlen("etag")));
    } else if (name_is(line, length, "last-modified")) {
        replace_text(&state->answer->last_modified, header_value(line, length, strlen("last-modified")));
    } else if (name_is(line, length, "content-length")) {
        take_declared_length(state, line, length);
    }
    return length;
}

static size_t take_body(char *bytes, size_t size, size_t count, void *context) {
    transfer *state = context;
    if (!is_success(state->status)) {
        state->refused_status = 1;
        return 0;
    }
    size_t length = size * count;
    return runtime_fetch_sink_take(state->sink, (const uint8_t *)bytes, length) ? length : 0;
}

static curl_list *conditional_headers(const curl_api *api, const runtime_fetch_request *request, int *built) {
    curl_list *headers = NULL;
    *built = 1;
    const char *names[2] = {"If-None-Match: ", "If-Modified-Since: "};
    const char *values[2] = {request->etag, request->last_modified};
    for (size_t index = 0; index < 2 && *built; index++) {
        if (values[index] == NULL) {
            continue;
        }
        size_t size = strlen(names[index]) + strlen(values[index]) + 1;
        char *line = malloc(size);
        if (line != NULL) {
            snprintf(line, size, "%s%s", names[index], values[index]);
        }
        curl_list *grown = line == NULL ? NULL : api->slist_append(headers, line);
        free(line);
        *built = grown != NULL;
        headers = grown != NULL ? grown : headers;
    }
    return headers;
}

static int configure(const curl_api *api, curl_handle *curl, const runtime_fetch_request *request, transfer *state,
                     curl_list *headers, char *error_buffer) {
    long protocols = request->allow_plain_http ? CURLPROTO_HTTP | CURLPROTO_HTTPS : CURLPROTO_HTTPS;
    long deadline = (long)request->deadline_milliseconds;
    long connect = deadline < CONNECT_MILLISECONDS ? deadline : CONNECT_MILLISECONDS;
    curl_data_function header_function = take_header;
    curl_data_function body_function = take_body;
    return api->easy_setopt(curl, CURLOPT_URL, request->url) == CURLE_OK &&
           api->easy_setopt(curl, CURLOPT_PROTOCOLS, protocols) == CURLE_OK &&
           api->easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, protocols) == CURLE_OK &&
           api->easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L) == CURLE_OK &&
           api->easy_setopt(curl, CURLOPT_MAXREDIRS, MAXIMUM_REDIRECTS) == CURLE_OK &&
           api->easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
           api->easy_setopt(curl, CURLOPT_NOPROGRESS, 1L) == CURLE_OK &&
           api->easy_setopt(curl, CURLOPT_TIMEOUT_MS, deadline) == CURLE_OK &&
           api->easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, connect) == CURLE_OK &&
           api->easy_setopt(curl, CURLOPT_USERAGENT, "spisor-runtime") == CURLE_OK &&
           api->easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer) == CURLE_OK &&
           api->easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_function) == CURLE_OK &&
           api->easy_setopt(curl, CURLOPT_HEADERDATA, (void *)state) == CURLE_OK &&
           api->easy_setopt(curl, CURLOPT_WRITEFUNCTION, body_function) == CURLE_OK &&
           api->easy_setopt(curl, CURLOPT_WRITEDATA, (void *)state) == CURLE_OK &&
           (headers == NULL || api->easy_setopt(curl, CURLOPT_HTTPHEADER, headers) == CURLE_OK);
}

static void settle_completed(transfer *state) {
    runtime_fetch_answer *answer = state->answer;
    answer->status = state->status;
    if (state->status == 304) {
        answer->outcome = RUNTIME_FETCH_NOT_MODIFIED;
    } else if (!is_success(state->status)) {
        answer->outcome = RUNTIME_FETCH_STATUS;
    } else if (state->declared_known && state->sink->bytes != state->declared) {
        answer->outcome = RUNTIME_FETCH_CUT_OFF;
        answer->declared = state->declared;
    } else {
        answer->outcome = RUNTIME_FETCH_RECEIVED;
    }
}

static void settle(const curl_api *api, const runtime_fetch_request *request, int code, transfer *state,
                   const char *error_buffer) {
    runtime_fetch_answer *answer = state->answer;
    if (code == CURLE_OK) {
        settle_completed(state);
    } else if (code == CURLE_WRITE_ERROR && state->refused_status) {
        answer->outcome = RUNTIME_FETCH_STATUS;
        answer->status = state->status;
    } else if (code == CURLE_WRITE_ERROR && (state->sink->exceeded || state->sink->unwritable)) {
        answer->outcome = RUNTIME_FETCH_STOPPED_BY_SINK;
    } else if (code == CURLE_PARTIAL_FILE && state->declared_known) {
        answer->outcome = RUNTIME_FETCH_CUT_OFF;
        answer->declared = state->declared;
    } else if (code == CURLE_OPERATION_TIMEDOUT) {
        runtime_fetch_refuse(answer, RUNTIME_FETCH_NETWORK_FIX, "%s did not finish within %u ms", request->url,
                             request->deadline_milliseconds);
    } else if (code == CURLE_TOO_MANY_REDIRECTS) {
        runtime_fetch_refuse(answer, "check that the catalog still publishes this file",
                             "%s redirected more than %ld times", request->url, MAXIMUM_REDIRECTS);
    } else {
        runtime_fetch_refuse(answer, RUNTIME_FETCH_NETWORK_FIX, "%s could not be fetched: %s", request->url,
                             error_buffer[0] != 0 ? error_buffer : api->easy_strerror(code));
    }
}

static void perform(const curl_api *api, curl_handle *curl, const runtime_fetch_request *request, transfer *state) {
    char error_buffer[ERROR_BUFFER_SIZE] = "";
    int built = 0;
    curl_list *headers = conditional_headers(api, request, &built);
    if (!built || !configure(api, curl, request, state, headers, error_buffer)) {
        runtime_fetch_refuse(state->answer, "report this as a defect in the runtime library",
                             "libcurl refused the options for %s", request->url);
    } else {
        settle(api, request, api->easy_perform(curl), state, error_buffer);
    }
    api->slist_free_all(headers);
}

void runtime_fetch_over_network(const runtime_fetch_request *request, runtime_fetch_sink *sink,
                                runtime_fetch_answer *answer) {
    const curl_api *api = load_api();
    if (api == NULL) {
        runtime_fetch_refuse(answer, MISSING_LIBCURL_FIX, "libcurl could not be loaded, so %s cannot be fetched",
                             request->url);
        return;
    }
    curl_handle *curl = api->easy_init();
    if (curl == NULL) {
        runtime_fetch_refuse(answer, RUNTIME_FETCH_NETWORK_FIX, "libcurl could not start a transfer for %s",
                             request->url);
        return;
    }
    transfer state = {sink, answer, 0, 0, 0, 0};
    perform(api, curl, request, &state);
    api->easy_cleanup(curl);
}
