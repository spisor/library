#include "catalog_url.h"

#include <stdlib.h>
#include <string.h>

#include "catalog_text.h"
#include "catalog_url_host.h"

#define HTTPS_DEFAULT_PORT 443
#define LARGEST_PORT 65535

typedef enum url_scheme {
    SCHEME_NONE,
    SCHEME_HTTPS,
    SCHEME_FILE,
    SCHEME_OTHER
} url_scheme;

typedef struct url_path {
    char **segments;
    size_t count;
    size_t capacity;
} url_path;

typedef struct parsed_url {
    url_scheme scheme;
    char *userinfo;
    char *host;
    char *port;
    url_path path;
    char *query;
    char *fragment;
} parsed_url;

typedef int (*encode_set)(unsigned char byte);

static int in_control_set(unsigned char byte) {
    return byte < 0x20 || byte > 0x7E;
}

static int in_fragment_set(unsigned char byte) {
    return in_control_set(byte) || strchr(" \"<>`", byte) != NULL;
}

static int in_query_set(unsigned char byte) {
    return in_control_set(byte) || strchr(" \"#<>'", byte) != NULL;
}

static int in_path_set(unsigned char byte) {
    return in_control_set(byte) || strchr(" \"#<>?^`{}", byte) != NULL;
}

static int in_userinfo_set(unsigned char byte) {
    return in_path_set(byte) || strchr("/:;=@[\\]|", byte) != NULL;
}

static char *encoded(const char *bytes, size_t length, encode_set in_set) {
    runtime_catalog_text text;
    runtime_catalog_text_start(&text);
    runtime_catalog_text_add(&text, "");
    for (size_t index = 0; index < length; index++) {
        unsigned char byte = (unsigned char)bytes[index];
        if (in_set(byte)) {
            runtime_catalog_text_add_escaped_byte(&text, byte);
        } else {
            runtime_catalog_text_add_character(&text, (char)byte);
        }
    }
    return runtime_catalog_text_finish(&text);
}

static int is_slash(char character) {
    return character == '/' || character == '\\';
}

static int is_letter(char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
}

static int is_windows_drive(const char *text, size_t length) {
    return length == 2 && is_letter(text[0]) && (text[1] == ':' || text[1] == '|');
}

static int is_normalized_windows_drive(const char *segment) {
    return strlen(segment) == 2 && is_letter(segment[0]) && segment[1] == ':';
}

static int starts_with_windows_drive(const char *text) {
    size_t length = strlen(text);
    return length >= 2 && is_windows_drive(text, 2) && (length == 2 || strchr("/\\?#", text[2]) != NULL);
}

static int is_dot_part(const char *text, size_t *consumed) {
    if (text[0] == '.') {
        *consumed = 1;
        return 1;
    }
    if (text[0] == '%' && text[1] == '2' && (text[2] == 'e' || text[2] == 'E')) {
        *consumed = 3;
        return 1;
    }
    return 0;
}

/** 1 for a single-dot segment, 2 for a double-dot one, 0 for any other, percent-encoded dots included. */
static int dots_in(const char *segment) {
    size_t first;
    if (!is_dot_part(segment, &first)) {
        return 0;
    }
    if (segment[first] == 0) {
        return 1;
    }
    size_t second;
    return is_dot_part(segment + first, &second) && segment[first + second] == 0 ? 2 : 0;
}

static int push_segment(url_path *path, char *segment) {
    if (segment == NULL) {
        return 0;
    }
    if (path->count == path->capacity) {
        size_t capacity = path->capacity == 0 ? 8 : path->capacity * 2;
        char **grown = realloc(path->segments, capacity * sizeof *grown);
        if (grown == NULL) {
            free(segment);
            return 0;
        }
        path->segments = grown;
        path->capacity = capacity;
    }
    path->segments[path->count++] = segment;
    return 1;
}

static void clear_path(url_path *path) {
    for (size_t index = 0; index < path->count; index++) {
        free(path->segments[index]);
    }
    path->count = 0;
}

static void free_url(parsed_url *url) {
    clear_path(&url->path);
    free(url->path.segments);
    free(url->userinfo);
    free(url->host);
    free(url->port);
    free(url->query);
    free(url->fragment);
    memset(url, 0, sizeof *url);
}

static char *copy_or_null(const char *text) {
    return text == NULL ? NULL : runtime_catalog_copy(text);
}

static int copy_path(url_path *to, const url_path *from) {
    for (size_t index = 0; index < from->count; index++) {
        if (!push_segment(to, runtime_catalog_copy(from->segments[index]))) {
            return 0;
        }
    }
    return 1;
}

static int copy_authority(parsed_url *url, const parsed_url *base) {
    url->userinfo = copy_or_null(base->userinfo);
    url->host = copy_or_null(base->host);
    url->port = copy_or_null(base->port);
    return (base->userinfo == NULL || url->userinfo != NULL) && url->host != NULL &&
           (base->port == NULL || url->port != NULL);
}

static void shorten_path(parsed_url *url) {
    url_path *path = &url->path;
    if (url->scheme == SCHEME_FILE && path->count == 1 && is_normalized_windows_drive(path->segments[0])) {
        return;
    }
    if (path->count > 0) {
        free(path->segments[--path->count]);
    }
}

static int parse_query_and_fragment(parsed_url *url, const char *at) {
    if (*at == '?') {
        const char *end = at + 1 + strcspn(at + 1, "#");
        free(url->query);
        url->query = encoded(at + 1, (size_t)(end - at - 1), in_query_set);
        if (url->query == NULL) {
            return 0;
        }
        at = end;
    }
    if (*at == '#') {
        url->fragment = encoded(at + 1, strlen(at + 1), in_fragment_set);
        return url->fragment != NULL;
    }
    return 1;
}

static int add_segment(parsed_url *url, char *segment, int ended_by_slash) {
    if (segment == NULL) {
        return 0;
    }
    int dots = dots_in(segment);
    if (dots == 2) {
        shorten_path(url);
    }
    if (dots != 0) {
        free(segment);
        return ended_by_slash || push_segment(&url->path, runtime_catalog_copy(""));
    }
    if (url->scheme == SCHEME_FILE && url->path.count == 0 && is_windows_drive(segment, strlen(segment))) {
        segment[1] = ':';
    }
    return push_segment(&url->path, segment);
}

static int parse_path(parsed_url *url, const char *at) {
    for (;;) {
        const char *end = at + strcspn(at, "/\\?#");
        int ended_by_slash = is_slash(*end);
        if (!add_segment(url, encoded(at, (size_t)(end - at), in_path_set), ended_by_slash)) {
            return 0;
        }
        if (!ended_by_slash) {
            return parse_query_and_fragment(url, end);
        }
        at = end + 1;
    }
}

static int parse_path_start(parsed_url *url, const char *at) {
    return parse_path(url, is_slash(*at) ? at + 1 : at);
}

static char *userinfo_of(const char *start, const char *end) {
    const char *colon = memchr(start, ':', (size_t)(end - start));
    const char *user_end = colon == NULL ? end : colon;
    char *user = encoded(start, (size_t)(user_end - start), in_userinfo_set);
    char *password = colon == NULL ? runtime_catalog_copy("")
                                   : encoded(colon + 1, (size_t)(end - colon - 1), in_userinfo_set);
    runtime_catalog_text text;
    runtime_catalog_text_start(&text);
    if (user == NULL || password == NULL) {
        text.failed = 1;
    } else {
        runtime_catalog_text_add(&text, user);
        if (password[0] != 0) {
            runtime_catalog_text_add_character(&text, ':');
            runtime_catalog_text_add(&text, password);
        }
    }
    free(user);
    free(password);
    return runtime_catalog_text_finish(&text);
}

static const char *port_colon(const char *start, const char *end) {
    int bracketed = 0;
    for (const char *at = start; at < end; at++) {
        if (*at == '[') {
            bracketed = 1;
        } else if (*at == ']') {
            bracketed = 0;
        } else if (*at == ':' && !bracketed) {
            return at;
        }
    }
    return NULL;
}

static int parse_port(parsed_url *url, const char *start, const char *end) {
    unsigned long value = 0;
    for (const char *at = start; at < end; at++) {
        if (*at < '0' || *at > '9') {
            return 0;
        }
        value = value * 10 + (unsigned long)(*at - '0');
        if (value > LARGEST_PORT) {
            return 0;
        }
    }
    if (start == end || value == HTTPS_DEFAULT_PORT) {
        return 1;
    }
    char digits[8];
    size_t length = 0;
    do {
        digits[length++] = (char)('0' + value % 10);
        value /= 10;
    } while (value > 0);
    url->port = malloc(length + 1);
    if (url->port == NULL) {
        return 0;
    }
    for (size_t index = 0; index < length; index++) {
        url->port[index] = digits[length - 1 - index];
    }
    url->port[length] = 0;
    return 1;
}

static int parse_authority(parsed_url *url, const char *at) {
    while (is_slash(*at)) {
        at++;
    }
    const char *end = at + strcspn(at, "/\\?#");
    const char *last_at = NULL;
    for (const char *scan = at; scan < end; scan++) {
        last_at = *scan == '@' ? scan : last_at;
    }
    if (last_at != NULL) {
        url->userinfo = userinfo_of(at, last_at);
        if (url->userinfo == NULL) {
            return 0;
        }
        if (url->userinfo[0] == 0) {
            free(url->userinfo);
            url->userinfo = NULL;
        }
    }
    const char *host_start = last_at == NULL ? at : last_at + 1;
    const char *colon = port_colon(host_start, end);
    const char *host_end = colon == NULL ? end : colon;
    if (host_end == host_start) {
        return 0;
    }
    url->host = runtime_catalog_parse_host(host_start, (size_t)(host_end - host_start));
    if (url->host == NULL || (colon != NULL && !parse_port(url, colon + 1, end))) {
        return 0;
    }
    return parse_path_start(url, end);
}

static int parse_https_relative(parsed_url *url, const char *at, const parsed_url *base) {
    if (is_slash(at[0]) && is_slash(at[1])) {
        return parse_authority(url, at);
    }
    if (!copy_authority(url, base)) {
        return 0;
    }
    if (is_slash(at[0])) {
        return parse_path_start(url, at);
    }
    url->query = copy_or_null(base->query);
    if (!copy_path(&url->path, &base->path) || (base->query != NULL && url->query == NULL)) {
        return 0;
    }
    if (*at == 0 || *at == '?' || *at == '#') {
        return parse_query_and_fragment(url, at);
    }
    free(url->query);
    url->query = NULL;
    shorten_path(url);
    return parse_path(url, at);
}

static int parse_file_host(parsed_url *url, const char *at) {
    const char *end = at + strcspn(at, "/\\?#");
    if (is_windows_drive(at, (size_t)(end - at))) {
        return parse_path(url, at);
    }
    if (end == at) {
        return parse_path_start(url, at);
    }
    char *host = runtime_catalog_parse_host(at, (size_t)(end - at));
    if (host == NULL) {
        return 0;
    }
    if (strcmp(host, "localhost") == 0) {
        host[0] = 0;
    }
    free(url->host);
    url->host = host;
    return parse_path_start(url, end);
}

static int parse_file_slash(parsed_url *url, const char *at, const parsed_url *base) {
    if (is_slash(*at)) {
        return parse_file_host(url, at + 1);
    }
    if (base != NULL) {
        free(url->host);
        url->host = runtime_catalog_copy(base->host);
        if (url->host == NULL) {
            return 0;
        }
        if (!starts_with_windows_drive(at) && base->path.count > 0 &&
            is_normalized_windows_drive(base->path.segments[0]) &&
            !push_segment(&url->path, runtime_catalog_copy(base->path.segments[0]))) {
            return 0;
        }
    }
    return parse_path(url, at);
}

static int parse_file_relative(parsed_url *url, const char *at, const parsed_url *base) {
    free(url->host);
    url->host = runtime_catalog_copy(base->host);
    url->query = copy_or_null(base->query);
    if (url->host == NULL || !copy_path(&url->path, &base->path) || (base->query != NULL && url->query == NULL)) {
        return 0;
    }
    if (*at == 0 || *at == '?' || *at == '#') {
        return parse_query_and_fragment(url, at);
    }
    free(url->query);
    url->query = NULL;
    if (starts_with_windows_drive(at)) {
        clear_path(&url->path);
    } else {
        shorten_path(url);
    }
    return parse_path(url, at);
}

static int parse_file(parsed_url *url, const char *at, const parsed_url *base) {
    url->host = runtime_catalog_copy("");
    if (url->host == NULL) {
        return 0;
    }
    if (is_slash(*at)) {
        return parse_file_slash(url, at + 1, base);
    }
    return base != NULL ? parse_file_relative(url, at, base) : parse_path(url, at);
}

/** The input without leading or trailing C0 controls and spaces, and without any tab or line break. */
static char *cleaned_input(const char *input) {
    const char *start = input;
    while (*start != 0 && (unsigned char)*start <= 0x20) {
        start++;
    }
    const char *end = start + strlen(start);
    while (end > start && (unsigned char)end[-1] <= 0x20) {
        end--;
    }
    char *cleaned = malloc((size_t)(end - start) + 1);
    if (cleaned == NULL) {
        return NULL;
    }
    size_t filled = 0;
    for (const char *at = start; at < end; at++) {
        if (*at != '\t' && *at != '\n' && *at != '\r') {
            cleaned[filled++] = *at;
        }
    }
    cleaned[filled] = 0;
    return cleaned;
}

static int is_scheme_character(char character) {
    return is_letter(character) || (character >= '0' && character <= '9') || character == '+' || character == '-' ||
           character == '.';
}

static int scheme_named(const char *start, size_t length, const char *name) {
    if (strlen(name) != length) {
        return 0;
    }
    for (size_t index = 0; index < length; index++) {
        if ((start[index] | 0x20) != name[index]) {
            return 0;
        }
    }
    return 1;
}

static url_scheme scheme_of(const char *input, const char **rest) {
    *rest = input;
    if (!is_letter(input[0])) {
        return SCHEME_NONE;
    }
    size_t length = 1;
    while (is_scheme_character(input[length])) {
        length++;
    }
    if (input[length] != ':') {
        return SCHEME_NONE;
    }
    *rest = input + length + 1;
    if (scheme_named(input, length, "https")) {
        return SCHEME_HTTPS;
    }
    return scheme_named(input, length, "file") ? SCHEME_FILE : SCHEME_OTHER;
}

static int parse_with_scheme(parsed_url *url, url_scheme scheme, const char *rest, const parsed_url *base) {
    url->scheme = scheme;
    int same_as_base = base != NULL && base->scheme == scheme;
    if (scheme == SCHEME_FILE) {
        return parse_file(url, rest, same_as_base ? base : NULL);
    }
    if (same_as_base && !(rest[0] == '/' && rest[1] == '/')) {
        return parse_https_relative(url, rest, base);
    }
    return parse_authority(url, rest);
}

static int parse_url(const char *input, const parsed_url *base, parsed_url *url) {
    memset(url, 0, sizeof *url);
    char *cleaned = cleaned_input(input);
    if (cleaned == NULL) {
        return 0;
    }
    const char *rest;
    url_scheme scheme = scheme_of(cleaned, &rest);
    int parsed;
    if (scheme == SCHEME_OTHER || (scheme == SCHEME_NONE && base == NULL)) {
        parsed = 0;
    } else if (scheme == SCHEME_NONE) {
        url->scheme = base->scheme;
        parsed = base->scheme == SCHEME_FILE ? parse_file(url, rest, base) : parse_https_relative(url, rest, base);
    } else {
        parsed = parse_with_scheme(url, scheme, rest, base);
    }
    free(cleaned);
    if (!parsed) {
        free_url(url);
    }
    return parsed;
}

static void add_path(runtime_catalog_text *text, const url_path *path) {
    for (size_t index = 0; index < path->count; index++) {
        runtime_catalog_text_add_character(text, '/');
        runtime_catalog_text_add(text, path->segments[index]);
    }
}

static char *pathname_of(const parsed_url *url) {
    runtime_catalog_text text;
    runtime_catalog_text_start(&text);
    runtime_catalog_text_add(&text, "");
    add_path(&text, &url->path);
    return runtime_catalog_text_finish(&text);
}

static char *href_of(const parsed_url *url) {
    runtime_catalog_text text;
    runtime_catalog_text_start(&text);
    runtime_catalog_text_add(&text, url->scheme == SCHEME_FILE ? "file://" : "https://");
    if (url->userinfo != NULL) {
        runtime_catalog_text_add(&text, url->userinfo);
        runtime_catalog_text_add_character(&text, '@');
    }
    runtime_catalog_text_add(&text, url->host);
    if (url->port != NULL) {
        runtime_catalog_text_add_character(&text, ':');
        runtime_catalog_text_add(&text, url->port);
    }
    add_path(&text, &url->path);
    if (url->query != NULL) {
        runtime_catalog_text_add_character(&text, '?');
        runtime_catalog_text_add(&text, url->query);
    }
    if (url->fragment != NULL) {
        runtime_catalog_text_add_character(&text, '#');
        runtime_catalog_text_add(&text, url->fragment);
    }
    return runtime_catalog_text_finish(&text);
}

static int is_inside_index_directory(const parsed_url *resolved, const parsed_url *index) {
    if (resolved->host[0] != 0 || index->host[0] != 0) {
        return 0;
    }
    char *resolved_path = pathname_of(resolved);
    char *index_path = pathname_of(index);
    int inside = 0;
    const char *last_slash = index_path == NULL ? NULL : strrchr(index_path, '/');
    if (resolved_path != NULL && last_slash != NULL) {
        size_t directory_length = (size_t)(last_slash - index_path) + 1;
        inside = strncmp(resolved_path, index_path, directory_length) == 0;
    }
    free(resolved_path);
    free(index_path);
    return inside;
}

char *runtime_catalog_resolve_within_origin(const char *url, const char *origin) {
    parsed_url base;
    if (!parse_url(origin, NULL, &base)) {
        return NULL;
    }
    parsed_url resolved;
    char *href = NULL;
    if (parse_url(url, &base, &resolved)) {
        int within = resolved.scheme == base.scheme &&
                     (base.scheme == SCHEME_HTTPS || is_inside_index_directory(&resolved, &base));
        href = within ? href_of(&resolved) : NULL;
        free_url(&resolved);
    }
    free_url(&base);
    return href;
}

static void add_path_characters(runtime_catalog_text *text, const char *path, int windows) {
    for (const char *at = path; *at != 0; at++) {
        int escaped = strchr("%\n\r\t#?", *at) != NULL || (!windows && *at == '\\');
        if (escaped) {
            runtime_catalog_text_add_escaped_byte(text, (unsigned char)*at);
        } else {
            runtime_catalog_text_add_character(text, *at);
        }
    }
}

static int add_unc_location(runtime_catalog_text *text, const char *path) {
    size_t prefix = strncmp(path, "\\\\?\\UNC\\", 8) == 0 ? 8 : 2;
    const char *host_end = strchr(path + prefix, '\\');
    if (host_end == NULL || host_end == path + 2) {
        return 0;
    }
    runtime_catalog_text_add_bytes(text, path + prefix, (size_t)(host_end - (path + prefix)));
    add_path_characters(text, host_end, 1);
    return 1;
}

char *runtime_catalog_file_url_of_path(int windows, const char *path) {
    runtime_catalog_text text;
    runtime_catalog_text_start(&text);
    runtime_catalog_text_add(&text, "file://");
    if (windows && strncmp(path, "\\\\", 2) == 0) {
        if (!add_unc_location(&text, path)) {
            runtime_catalog_text_discard(&text);
            return NULL;
        }
    } else {
        runtime_catalog_text_add(&text, windows ? "/" : "");
        add_path_characters(&text, path, windows);
    }
    char *written = runtime_catalog_text_finish(&text);
    if (written == NULL) {
        return NULL;
    }
    parsed_url url;
    char *href = NULL;
    if (parse_url(written, NULL, &url)) {
        href = href_of(&url);
        free_url(&url);
    }
    free(written);
    return href;
}
