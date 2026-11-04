#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "directory_tree.h"
#include "fetch.h"
#include "fetch_hooks.h"
#include "http_fixture.h"
#include "scratch_directory.h"
#include "sha256.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#include <winhttp.h>
#define current_process_id() _getpid()
#else
#include <pthread.h>
#include <unistd.h>
#define current_process_id() getpid()
#endif

#define BODY_LENGTH 100000
#define CUT_BODY_LENGTH 500
#define ETAG "\"one\""
#define LAST_MODIFIED "Wed, 21 Oct 2015 07:28:00 GMT"
#define WRONG_SHA256 "0000000000000000000000000000000000000000000000000000000000000000"

typedef struct answer {
    int32_t status;
    cJSON *tree;
} answer;

static char body[BODY_LENGTH];
static char body_sha256[RUNTIME_SHA256_HEX_SIZE];
static char scratch[1024];
static char destination[1200];
static unsigned short server_port;

static void prepare_body(void) {
    for (size_t index = 0; index < BODY_LENGTH; index++) {
        body[index] = (char)('a' + index % 26);
    }
    runtime_sha256 hash;
    runtime_sha256_start(&hash);
    runtime_sha256_add(&hash, (const uint8_t *)body, BODY_LENGTH);
    runtime_sha256_finish_hex(&hash, body_sha256);
}

static void start_scratch(void) {
    snprintf(scratch, sizeof scratch, "%s/fetch-J\xc3\xb6rg-%d", RUNTIME_SCRATCH_PATH, (int)current_process_id());
    start_empty_directory(scratch);
    snprintf(destination, sizeof destination, "%s/cache/bundles/file.bin", scratch);
}

static char *http_url(const char *path) {
    char *url = malloc(256);
    snprintf(url, 256, "http://127.0.0.1:%u%s", (unsigned)server_port, path);
    return url;
}

static cJSON *request_for(const char *url) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "url", url);
    cJSON_AddStringToObject(request, "destination", destination);
    return request;
}

static cJSON *request_for_path(const char *path) {
    char *url = http_url(path);
    cJSON *request = request_for(url);
    free(url);
    return request;
}

static answer get(cJSON *request) {
    char *text = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    uint8_t *out = NULL;
    size_t out_len = 0;
    answer result;
    result.status = runtime_fetch_get((const uint8_t *)text, strlen(text), &out, &out_len);
    free(text);
    result.tree = out == NULL ? NULL : spisor_parse_json(out, out_len);
    free(out);
    return result;
}

static const char *text_of(const cJSON *tree, const char *member) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(tree, member);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static int has_no_partial_file(void) {
    char parent[1200];
    snprintf(parent, sizeof parent, "%s/cache/bundles", scratch);
    runtime_directory_listing listing;
    if (!runtime_list_directory(parent, &listing)) {
        return 0;
    }
    int clean = 1;
    for (size_t index = 0; index < listing.count; index++) {
        if (strstr(listing.entries[index].name, ".partial-") != NULL) {
            fprintf(stderr, "left behind: %s\n", listing.entries[index].name);
            clean = 0;
        }
    }
    runtime_free_directory_listing(&listing);
    return clean;
}

static void expect_nothing_stored(const char *label) {
    if (spisor_file_exists(destination)) {
        fprintf(stderr, "%s: the destination exists\n", label);
        check_failures++;
    }
    if (!has_no_partial_file()) {
        fprintf(stderr, "%s: a partial file is left\n", label);
        check_failures++;
    }
}

static void expect_refused(answer result, const char *fragment, const char *label) {
    const char *message = text_of(result.tree, "message");
    if (result.status != SPISOR_STATUS_FAILURE || message == NULL || strstr(message, fragment) == NULL ||
        text_of(result.tree, "fix") == NULL) {
        fprintf(stderr, "%s: expected a refusal naming \"%s\", got status %d and \"%s\"\n", label, fragment,
                (int)result.status, message == NULL ? "(none)" : message);
        check_failures++;
    }
    cJSON_Delete(result.tree);
}

static void expect_stored_body(answer result, const char *label) {
    CHECK(result.status == SPISOR_STATUS_SUCCESS);
    check_text(text_of(result.tree, "outcome"), "stored", label);
    check_text(text_of(result.tree, "sha256"), body_sha256, label);
    const cJSON *bytes = cJSON_GetObjectItemCaseSensitive(result.tree, "bytes");
    CHECK(cJSON_IsNumber(bytes) && bytes->valuedouble == BODY_LENGTH);
    char stored_sha256[RUNTIME_SHA256_HEX_SIZE] = "";
    CHECK(spisor_sha256_file(destination, stored_sha256) == 1);
    check_text(stored_sha256, body_sha256, label);
    CHECK(has_no_partial_file());
    cJSON_Delete(result.tree);
}

static void remove_destination(void) {
    CHECK(runtime_remove_file_entry(destination) == RUNTIME_ENTRY_DONE);
}

static void a_200_stores_the_file_with_its_hash_and_validators(void) {
    cJSON *request = request_for_path("/file");
    cJSON_AddStringToObject(request, "sha256", body_sha256);
    answer result = get(request);
    check_text(text_of(result.tree, "etag"), ETAG, "etag");
    check_text(text_of(result.tree, "lastModified"), LAST_MODIFIED, "lastModified");
    expect_stored_body(result, "200");
    remove_destination();
}

static void a_stored_file_replaces_what_stood_at_the_destination(void) {
    expect_stored_body(get(request_for_path("/file")), "first");
    CHECK(spisor_replace_file(destination, (const uint8_t *)"old", 3) == 1);
    expect_stored_body(get(request_for_path("/file")), "replacing");
    remove_destination();
}

static void a_304_against_the_sent_etag_stores_nothing(void) {
    cJSON *request = request_for_path("/file");
    cJSON_AddStringToObject(request, "etag", ETAG);
    answer result = get(request);
    CHECK(result.status == SPISOR_STATUS_SUCCESS);
    check_text(text_of(result.tree, "outcome"), "not-modified", "304 etag");
    CHECK(cJSON_GetArraySize(result.tree) == 1);
    cJSON_Delete(result.tree);
    expect_nothing_stored("304 etag");
}

static void a_304_against_the_sent_last_modified_stores_nothing(void) {
    cJSON *request = request_for_path("/file");
    cJSON_AddStringToObject(request, "lastModified", LAST_MODIFIED);
    answer result = get(request);
    CHECK(result.status == SPISOR_STATUS_SUCCESS);
    check_text(text_of(result.tree, "outcome"), "not-modified", "304 last modified");
    cJSON_Delete(result.tree);
    expect_nothing_stored("304 last modified");
}

static void a_304_to_a_request_with_no_validator_is_refused(void) {
    expect_refused(get(request_for_path("/always-304")), "answered 304", "unasked 304");
    expect_nothing_stored("unasked 304");
}

static void a_mismatched_sha256_is_refused_and_leaves_nothing(void) {
    cJSON *request = request_for_path("/file");
    cJSON_AddStringToObject(request, "sha256", WRONG_SHA256);
    expect_refused(get(request), body_sha256, "mismatch");
    expect_nothing_stored("mismatch");
}

static void a_body_cut_off_mid_stream_is_refused_and_leaves_nothing(void) {
    expect_refused(get(request_for_path("/cut")), "ended after 500 of the 1000 bytes", "cut without hash");
    expect_nothing_stored("cut without hash");
    cJSON *request = request_for_path("/cut");
    cJSON_AddStringToObject(request, "sha256", body_sha256);
    expect_refused(get(request), "ended after 500 of the 1000 bytes", "cut with hash");
    expect_nothing_stored("cut with hash");
}

static void a_declared_length_over_max_bytes_is_refused(void) {
    cJSON *request = request_for_path("/file");
    cJSON_AddNumberToObject(request, "maxBytes", BODY_LENGTH - 1);
    expect_refused(get(request), "served more than the 99999 bytes", "declared over");
    expect_nothing_stored("declared over");
    request = request_for_path("/cut");
    cJSON_AddNumberToObject(request, "maxBytes", 999);
    expect_refused(get(request), "served more than the 999 bytes", "declared over, refused before the body");
    expect_nothing_stored("declared over, refused before the body");
}

static void an_undeclared_body_over_max_bytes_is_refused(void) {
    cJSON *request = request_for_path("/streamed");
    cJSON_AddNumberToObject(request, "maxBytes", 1000);
    expect_refused(get(request), "served more than the 1000 bytes", "streamed over");
    expect_nothing_stored("streamed over");
}

static void a_body_of_exactly_max_bytes_is_stored(void) {
    cJSON *request = request_for_path("/streamed");
    cJSON_AddNumberToObject(request, "maxBytes", BODY_LENGTH);
    expect_stored_body(get(request), "exactly max");
    remove_destination();
}

static void another_status_is_refused_naming_it(void) {
    expect_refused(get(request_for_path("/missing")), "answered HTTP 404", "404");
    expect_nothing_stored("404");
}

static void a_fetch_outliving_its_timeout_is_refused(void) {
    cJSON *request = request_for_path("/stall");
    cJSON_AddNumberToObject(request, "timeoutSeconds", 1);
    expect_refused(get(request), "did not finish within 1000 ms", "timeoutSeconds");
    expect_nothing_stored("timeoutSeconds");
    cJSON *unusable = request_for_path("/stall");
    cJSON_AddNumberToObject(unusable, "timeoutSeconds", 0);
    expect_refused(get(unusable), "timeoutSeconds", "timeoutSeconds zero");
}

static void a_fetch_outliving_its_deadline_is_refused(void) {
    runtime_fetch_use_deadline(300);
    expect_refused(get(request_for_path("/stall")), "did not finish within 300 ms", "deadline");
    runtime_fetch_use_deadline(0);
    expect_nothing_stored("deadline");
}

static void a_relative_redirect_is_followed(void) {
    expect_stored_body(get(request_for_path("/moved")), "redirect");
    remove_destination();
}

static size_t occurrences(const char *text, const char *needle) {
    size_t count = 0;
    for (const char *found = strstr(text, needle); found != NULL; found = strstr(found + 1, needle)) {
        count++;
    }
    return count;
}

static void a_redirect_loop_is_refused(void) {
    http_route loop = {"/loop", 302, "Location: /loop\r\n", "", 0, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0};
    http_fixture *fixture = http_fixture_start(&loop, 1);
    CHECK(fixture != NULL);
    char url[256];
    snprintf(url, sizeof url, "http://127.0.0.1:%u/loop", (unsigned)http_fixture_port(fixture));
    expect_refused(get(request_for(url)), "redirected more than 5 times", "loop");
    char *log = http_fixture_stop(fixture);
    CHECK(log != NULL && occurrences(log, "GET /loop ") <= 6);
    free(log);
    expect_nothing_stored("loop");
}

#if defined(_WIN32)
static void a_destination_held_open_with_the_same_bytes_is_accepted(void) {
    expect_stored_body(get(request_for_path("/file")), "before holding");
    wchar_t wide[1300];
    CHECK(MultiByteToWideChar(CP_UTF8, 0, destination, -1, wide, 1300) > 0);
    HANDLE held = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(held != INVALID_HANDLE_VALUE);
    expect_stored_body(get(request_for_path("/file")), "held open");
    CloseHandle(held);
    remove_destination();
}
#endif

typedef struct concurrent_fetch {
    answer result;
} concurrent_fetch;

static void fetch_slowly(concurrent_fetch *fetch) {
    fetch->result = get(request_for_path("/slow"));
}

#if defined(_WIN32)
static DWORD WINAPI fetching_entry(LPVOID argument) {
    fetch_slowly(argument);
    return 0;
}
#else
static void *fetching_entry(void *argument) {
    fetch_slowly(argument);
    return NULL;
}
#endif

static void two_threads_fetching_one_destination_both_store_it(void) {
    concurrent_fetch fetches[2];
    memset(fetches, 0, sizeof fetches);
#if defined(_WIN32)
    HANDLE threads[2];
    threads[0] = CreateThread(NULL, 0, fetching_entry, &fetches[0], 0, NULL);
    threads[1] = CreateThread(NULL, 0, fetching_entry, &fetches[1], 0, NULL);
    CHECK(threads[0] != NULL && threads[1] != NULL);
    WaitForMultipleObjects(2, threads, TRUE, INFINITE);
    CloseHandle(threads[0]);
    CloseHandle(threads[1]);
#else
    pthread_t threads[2];
    CHECK(pthread_create(&threads[0], NULL, fetching_entry, &fetches[0]) == 0);
    CHECK(pthread_create(&threads[1], NULL, fetching_entry, &fetches[1]) == 0);
    pthread_join(threads[0], NULL);
    pthread_join(threads[1], NULL);
#endif
    expect_stored_body(fetches[0].result, "first thread");
    expect_stored_body(fetches[1].result, "second thread");
    remove_destination();
}

static void a_redirect_to_a_file_url_is_refused(void) {
    expect_refused(get(request_for_path("/to-file")), "redirected to a url it may not follow", "redirect to file");
    expect_nothing_stored("redirect to file");
}

static void append_percent_encoded(char *url, size_t capacity, const char *path) {
    size_t length = strlen(url);
    for (const unsigned char *at = (const unsigned char *)path; *at != 0 && length + 4 < capacity; at++) {
        unsigned char character = *at == '\\' ? '/' : *at;
        int plain = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                    (character >= '0' && character <= '9') || strchr("/-._~:", character) != NULL;
        length += (size_t)(plain ? snprintf(url + length, capacity - length, "%c", character)
                                 : snprintf(url + length, capacity - length, "%%%02X", character));
    }
}

static void file_url_of(const char *path, char *url, size_t capacity) {
    snprintf(url, capacity, path[0] == '/' ? "file://" : "file:///");
    append_percent_encoded(url, capacity, path);
}

static void a_file_url_is_copied_through_the_same_verification(void) {
    char source[1200];
    snprintf(source, sizeof source, "%s/local catalog.bin", scratch);
    CHECK(spisor_replace_file(source, (const uint8_t *)body, BODY_LENGTH) == 1);
    char url[2400];
    file_url_of(source, url, sizeof url);
    cJSON *request = request_for(url);
    cJSON_AddStringToObject(request, "sha256", body_sha256);
    answer result = get(request);
    CHECK(cJSON_GetObjectItemCaseSensitive(result.tree, "etag") == NULL);
    expect_stored_body(result, "file url");
    remove_destination();
    request = request_for(url);
    cJSON_AddStringToObject(request, "sha256", WRONG_SHA256);
    expect_refused(get(request), body_sha256, "file url mismatch");
    expect_nothing_stored("file url mismatch");
    request = request_for(url);
    cJSON_AddNumberToObject(request, "maxBytes", 10);
    expect_refused(get(request), "served more than the 10 bytes", "file url over");
    expect_nothing_stored("file url over");
}

static void a_file_url_that_cannot_be_read_is_refused(void) {
    char source[1200];
    char url[2400];
    snprintf(source, sizeof source, "%s/absent.bin", scratch);
    file_url_of(source, url, sizeof url);
    expect_refused(get(request_for(url)), "could not be read", "absent file url");
    expect_nothing_stored("absent file url");
}

static void a_file_url_with_an_encoded_separator_is_refused(void) {
    expect_refused(get(request_for("file:///tmp/a%2Fb")), "encoded path separator", "encoded slash");
    expect_refused(get(request_for("file:///tmp/a%2fb")), "encoded path separator", "encoded lowercase slash");
#if defined(_WIN32)
    expect_refused(get(request_for("file:///C:/a%5Cb")), "encoded path separator", "encoded backslash");
#endif
    expect_nothing_stored("encoded separator");
}

static void a_file_url_naming_a_host_is_refused(void) {
    expect_refused(get(request_for("file://server/share/file.bin")), "names the host server", "file host");
    expect_nothing_stored("file host");
}

static void any_other_scheme_is_refused(void) {
    expect_refused(get(request_for("ftp://example.com/file.bin")), "uses the scheme ftp:", "ftp");
    expect_refused(get(request_for("no scheme at all")), "uses the scheme (none)", "no scheme");
    runtime_fetch_allow_plain_http(0);
    expect_refused(get(request_for_path("/file")), "uses the scheme http:", "plain http");
    runtime_fetch_allow_plain_http(1);
    expect_nothing_stored("other scheme");
}

static void an_unusable_argument_is_refused(void) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "destination", destination);
    expect_refused(get(request), "url", "no url");
    request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "url", "https://example.com/x");
    cJSON_AddStringToObject(request, "destination", "relative/file.bin");
    expect_refused(get(request), "destination", "relative destination");
    request = request_for_path("/file");
    cJSON_AddStringToObject(request, "sha256", "ABC");
    expect_refused(get(request), "sha256", "bad sha256");
    request = request_for_path("/file");
    cJSON_AddNumberToObject(request, "maxBytes", -1);
    expect_refused(get(request), "maxBytes", "negative maxBytes");
    request = request_for_path("/file");
    cJSON_AddStringToObject(request, "etag", "\"one\"\r\nX-Injected: 1");
    expect_refused(get(request), "etag", "etag with a line break");
    expect_nothing_stored("unusable argument");
}

#if defined(_WIN32)
static void winhttp_takes_the_system_proxy(void) {
    CHECK(runtime_fetch_windows_access_type() == WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY);
}
#else
static void libcurl_honours_https_proxy_and_no_proxy(void) {
    http_fixture *proxy = http_fixture_start_proxy();
    CHECK(proxy != NULL);
    char proxy_url[64];
    snprintf(proxy_url, sizeof proxy_url, "http://127.0.0.1:%u", (unsigned)http_fixture_port(proxy));
    setenv("HTTPS_PROXY", proxy_url, 1);
    expect_refused(get(request_for("https://catalog.invalid/index.json")), "502", "through the proxy");
    setenv("NO_PROXY", "catalog.invalid", 1);
    expect_refused(get(request_for("https://catalog.invalid/index.json")), "resolve host", "past the proxy");
    unsetenv("NO_PROXY");
    unsetenv("HTTPS_PROXY");
    char *log = http_fixture_stop(proxy);
    CHECK(log != NULL && occurrences(log, "CONNECT catalog.invalid:443 HTTP/1.1") == 1);
    free(log);
    expect_nothing_stored("proxy");
}

static void clear_proxy_environment(void) {
    const char *names[] = {"http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY", "all_proxy",
                           "ALL_PROXY",  "no_proxy",   "NO_PROXY"};
    for (size_t index = 0; index < sizeof names / sizeof names[0]; index++) {
        unsetenv(names[index]);
    }
}
#endif

#if defined(__linux__)
static void a_linux_without_libcurl_is_refused_naming_the_package(void) {
    runtime_fetch_curl_use_library("libspisor-no-such-curl.so.4");
    answer result = get(request_for_path("/file"));
    CHECK(result.status == SPISOR_STATUS_FAILURE);
    check_text(text_of(result.tree, "fix"), "install the libcurl package (libcurl4 on Debian and Ubuntu)",
               "libcurl fix");
    cJSON_Delete(result.tree);
    runtime_fetch_curl_use_library(NULL);
    expect_nothing_stored("no libcurl");
}
#endif

static void run_against_the_server(void) {
    char cut_body[CUT_BODY_LENGTH];
    memset(cut_body, 'c', sizeof cut_body);
    const http_route routes[] = {
        {"/file", 200, NULL, body, BODY_LENGTH, HTTP_ROUTE_BODY_LENGTH, ETAG, LAST_MODIFIED, 0},
        {"/always-304", 304, NULL, "", 0, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0},
        {"/cut", 200, NULL, cut_body, CUT_BODY_LENGTH, 1000, NULL, NULL, 0},
        {"/streamed", 200, NULL, body, BODY_LENGTH, HTTP_ROUTE_UNTIL_CLOSE, NULL, NULL, 0},
        {"/stall", 200, NULL, body, BODY_LENGTH, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 1500},
        {"/slow", 200, NULL, body, BODY_LENGTH, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 300},
        {"/moved", 302, "Location: /file\r\n", "", 0, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0},
        {"/to-file", 302, "Location: file:///etc/hosts\r\n", "", 0, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0},
    };
    http_fixture *server = http_fixture_start(routes, sizeof routes / sizeof routes[0]);
    CHECK(server != NULL);
    if (server == NULL) {
        return;
    }
    server_port = http_fixture_port(server);
    a_200_stores_the_file_with_its_hash_and_validators();
    a_stored_file_replaces_what_stood_at_the_destination();
    a_304_against_the_sent_etag_stores_nothing();
    a_304_against_the_sent_last_modified_stores_nothing();
    a_304_to_a_request_with_no_validator_is_refused();
    a_mismatched_sha256_is_refused_and_leaves_nothing();
    a_body_cut_off_mid_stream_is_refused_and_leaves_nothing();
    a_declared_length_over_max_bytes_is_refused();
    an_undeclared_body_over_max_bytes_is_refused();
    a_body_of_exactly_max_bytes_is_stored();
    another_status_is_refused_naming_it();
    a_fetch_outliving_its_deadline_is_refused();
    a_fetch_outliving_its_timeout_is_refused();
    a_relative_redirect_is_followed();
    a_redirect_to_a_file_url_is_refused();
#if defined(_WIN32)
    a_destination_held_open_with_the_same_bytes_is_accepted();
#endif
    two_threads_fetching_one_destination_both_store_it();
    any_other_scheme_is_refused();
    an_unusable_argument_is_refused();
#if defined(__linux__)
    a_linux_without_libcurl_is_refused_naming_the_package();
#endif
    free(http_fixture_stop(server));
}

int main(void) {
#if !defined(_WIN32)
    clear_proxy_environment();
#endif
    prepare_body();
    start_scratch();
    runtime_fetch_allow_plain_http(1);
    run_against_the_server();
    a_redirect_loop_is_refused();
    a_file_url_is_copied_through_the_same_verification();
    a_file_url_that_cannot_be_read_is_refused();
    a_file_url_with_an_encoded_separator_is_refused();
    a_file_url_naming_a_host_is_refused();
#if defined(_WIN32)
    winhttp_takes_the_system_proxy();
#else
    libcurl_honours_https_proxy_and_no_proxy();
#endif
    CHECK(runtime_remove_tree(scratch) == 1);
    return check_summary("fetch_test");
}
