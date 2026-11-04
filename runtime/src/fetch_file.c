#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "fetch_transport.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#include "fetch_hooks.h"
#include "file_stream.h"
#include "paths.h"

#define FILE_CHUNK 65536
#define FILE_URL_READ_FIX "check that the local catalog's files are where its index says"

static int is_hex_digit(char character) {
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
           (character >= 'A' && character <= 'F');
}

static int hex_value(char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    return (character | 0x20) - 'a' + 10;
}

static int is_separator_byte(int byte) {
    return byte == '/' || (RUNTIME_HOST_IS_WINDOWS && byte == '\\');
}

typedef enum decoding {
    DECODED,
    ENCODED_SEPARATOR,
    UNDECODABLE
} decoding;

static decoding percent_decode(const char *encoded, size_t length, char *decoded) {
    size_t written = 0;
    for (size_t index = 0; index < length; index++) {
        if (encoded[index] != '%') {
            decoded[written++] = encoded[index];
            continue;
        }
        if (index + 2 >= length || !is_hex_digit(encoded[index + 1]) || !is_hex_digit(encoded[index + 2])) {
            return UNDECODABLE;
        }
        int byte = hex_value(encoded[index + 1]) * 16 + hex_value(encoded[index + 2]);
        if (is_separator_byte(byte)) {
            return ENCODED_SEPARATOR;
        }
        if (byte == 0) {
            return UNDECODABLE;
        }
        decoded[written++] = (char)byte;
        index += 2;
    }
    decoded[written] = 0;
    return DECODED;
}

#if defined(_WIN32)
static int is_drive_path(const char *path) {
    char letter = (char)(path[1] | 0x20);
    return path[0] == '/' && letter >= 'a' && letter <= 'z' && path[2] == ':' && (path[3] == '/' || path[3] == 0);
}

static int make_local(char *decoded) {
    if (!is_drive_path(decoded)) {
        return 0;
    }
    memmove(decoded, decoded + 1, strlen(decoded));
    for (char *at = decoded; *at != 0; at++) {
        *at = *at == '/' ? '\\' : *at;
    }
    return 1;
}
#else
static int make_local(char *decoded) {
    return decoded[0] == '/';
}
#endif

/**
 * @implNote Matches Node's fileURLToPath, which the TypeScript cache decoded with: an encoded "/" (and on
 * Windows an encoded "\") is refused rather than decoded, so a url can never name a different path than
 * the one a containment check saw, and on Windows the path must start with a drive letter.
 */
static char *local_path_of(const char *url, const char *path, runtime_fetch_answer *answer) {
    size_t length = strcspn(path, "?#");
    char *decoded = malloc(length + 1);
    if (decoded == NULL) {
        runtime_fetch_refuse(answer, FILE_URL_READ_FIX, "%s could not be read: out of memory", url);
        return NULL;
    }
    decoding result = percent_decode(path, length, decoded);
    if (result != DECODED) {
        free(decoded);
        if (result == ENCODED_SEPARATOR) {
            runtime_fetch_refuse(answer, "name the file by a file url whose path separators are plain slashes",
                                 "%s carries an encoded path separator, which could name a different path", url);
        } else {
            runtime_fetch_refuse(answer, FILE_URL_READ_FIX, "%s carries an escape that names no file", url);
        }
        return NULL;
    }
    if (!make_local(decoded)) {
        free(decoded);
        runtime_fetch_refuse(answer, FILE_URL_READ_FIX, "%s names no absolute path on this system", url);
        return NULL;
    }
    return decoded;
}

static int is_local_host(const char *host, size_t length) {
    static const char localhost[] = "localhost";
    if (length == 0) {
        return 1;
    }
    if (length != sizeof localhost - 1) {
        return 0;
    }
    for (size_t index = 0; index < length; index++) {
        if ((host[index] | 0x20) != localhost[index]) {
            return 0;
        }
    }
    return 1;
}

static const char *path_after_authority(const char *url, runtime_fetch_answer *answer) {
    const char *rest = url + strlen("file:");
    if (strncmp(rest, "//", 2) != 0) {
        return rest;
    }
    const char *host = rest + 2;
    size_t host_length = strcspn(host, "/?#");
    if (!is_local_host(host, host_length)) {
        runtime_fetch_refuse(answer, "serve remote files over https and local ones by a file url with no host",
                             "%s names the host %.*s, which would read from a network share", url, (int)host_length,
                             host);
        return NULL;
    }
    return host + host_length;
}

static void copy_into_sink(FILE *file, const char *url, runtime_fetch_sink *sink, runtime_fetch_answer *answer) {
    uint8_t *chunk = malloc(FILE_CHUNK);
    if (chunk == NULL) {
        runtime_fetch_refuse(answer, FILE_URL_READ_FIX, "%s could not be read: out of memory", url);
        return;
    }
    size_t read;
    int taken = 1;
    while (taken && (read = fread(chunk, 1, FILE_CHUNK, file)) > 0) {
        taken = runtime_fetch_sink_take(sink, chunk, read);
    }
    free(chunk);
    if (!taken) {
        answer->outcome = RUNTIME_FETCH_STOPPED_BY_SINK;
    } else if (ferror(file)) {
        runtime_fetch_refuse(answer, FILE_URL_READ_FIX, "%s could not be read to its end", url);
    } else {
        answer->outcome = RUNTIME_FETCH_RECEIVED;
    }
}

#ifdef SPISOR_FETCH_TEST_HOOKS
static const char *stalled_url_part = NULL;
static unsigned stall_milliseconds = 0;

void runtime_fetch_stall_file_urls(const char *containing, unsigned milliseconds) {
    stalled_url_part = containing;
    stall_milliseconds = milliseconds;
}

static void stall_if_asked(const char *url) {
    if (stalled_url_part == NULL || strstr(url, stalled_url_part) == NULL) {
        return;
    }
#if defined(_WIN32)
    Sleep(stall_milliseconds);
#else
    struct timespec pause = {(time_t)(stall_milliseconds / 1000), (long)(stall_milliseconds % 1000) * 1000000L};
    nanosleep(&pause, NULL);
#endif
}
#else
static void stall_if_asked(const char *url) {
    (void)url;
}
#endif

void runtime_fetch_file_url(const char *url, runtime_fetch_sink *sink, runtime_fetch_answer *answer) {
    const char *path = path_after_authority(url, answer);
    char *local = path == NULL ? NULL : local_path_of(url, path, answer);
    if (local == NULL) {
        return;
    }
    stall_if_asked(url);
    FILE *file = runtime_open_file(local, RUNTIME_FILE_READ);
    free(local);
    if (file == NULL) {
        runtime_fetch_refuse(answer, FILE_URL_READ_FIX, "%s could not be read", url);
        return;
    }
    copy_into_sink(file, url, sink, answer);
    fclose(file);
}
