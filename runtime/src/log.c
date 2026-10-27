#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "paths.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"
#include "spisor/path.h"

#define ISO_LENGTH 24
#define ANSI_RESET "\x1b[0m"
#define ANSI_RED 31

typedef enum log_level { LEVEL_DEBUG, LEVEL_INFO, LEVEL_WARN, LEVEL_ERROR, LEVEL_UNKNOWN } log_level;

typedef struct log_entry {
    const char *home;
    const char *plugin;
    const char *start;
    const char *time;
    const char *message;
    const cJSON *names;
    const cJSON *config;
    log_level level;
    int no_color;
} log_entry;

typedef struct console_style {
    int enabled;
    int colored;
} console_style;

static const uint32_t PALETTE[] = {36, 32, 33, 35, 34, 96, 92, 93, 95, 94};
static const size_t PALETTE_SIZE = sizeof PALETTE / sizeof PALETTE[0];
static const char *const LEVEL_TAGS[] = {"DEBUG", "INFO", "WARN", "ERROR"};

static uint32_t continuation_bits(const unsigned char *bytes, size_t count) {
    uint32_t bits = 0;
    for (size_t index = 0; index < count; index++) {
        bits = (bits << 6) | (bytes[index] & 0x3Fu);
    }
    return bits;
}

static size_t decode_code_point(const unsigned char *bytes, uint32_t *code_point) {
    if (bytes[0] < 0x80) {
        *code_point = bytes[0];
        return 1;
    }
    if (bytes[0] < 0xE0) {
        *code_point = ((bytes[0] & 0x1Fu) << 6) | continuation_bits(bytes + 1, 1);
        return 2;
    }
    if (bytes[0] < 0xF0) {
        *code_point = ((bytes[0] & 0x0Fu) << 12) | continuation_bits(bytes + 1, 2);
        return 3;
    }
    *code_point = ((bytes[0] & 0x07u) << 18) | continuation_bits(bytes + 1, 3);
    return 4;
}

static uint32_t hash_unit(uint32_t hash, uint32_t unit) {
    return hash * 31u + unit;
}

static uint32_t hash_code_point(uint32_t hash, uint32_t code_point) {
    if (code_point < 0x10000u) {
        return hash_unit(hash, code_point);
    }
    uint32_t offset = code_point - 0x10000u;
    return hash_unit(hash_unit(hash, 0xD800u + (offset >> 10)), 0xDC00u + (offset & 0x3FFu));
}

uint32_t runtime_name_colour(const char *name) {
    const unsigned char *bytes = (const unsigned char *)name;
    uint32_t hash = 0;
    while (*bytes != '\0') {
        uint32_t code_point;
        bytes += decode_code_point(bytes, &code_point);
        hash = hash_code_point(hash, code_point);
    }
    return PALETTE[hash % PALETTE_SIZE];
}

static log_level level_named(const char *text) {
    const char *const names[] = {"debug", "info", "warn", "error"};
    for (int index = 0; index < 4; index++) {
        if (strcmp(text, names[index]) == 0) {
            return (log_level)index;
        }
    }
    return LEVEL_UNKNOWN;
}

static int is_digits(const char *text, size_t count) {
    for (size_t index = 0; index < count; index++) {
        if (text[index] < '0' || text[index] > '9') {
            return 0;
        }
    }
    return 1;
}

static int is_iso_time(const char *text) {
    return strlen(text) == ISO_LENGTH && is_digits(text, 4) && text[4] == '-' && is_digits(text + 5, 2) &&
           text[7] == '-' && is_digits(text + 8, 2) && text[10] == 'T' && is_digits(text + 11, 2) &&
           text[13] == ':' && is_digits(text + 14, 2) && text[16] == ':' && is_digits(text + 17, 2) &&
           text[19] == '.' && is_digits(text + 20, 3) && text[23] == 'Z';
}

static const char *string_member(const cJSON *object, const char *member) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, member);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static const cJSON *optional_object(const cJSON *parent, const char *member) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, member);
    return cJSON_IsObject(item) ? item : NULL;
}

static int32_t refuse_argument(uint8_t **out, size_t *out_len) {
    return spisor_fail("the argument is not a well formed log entry",
                       "send string home, plugin, start, time, level and message, a boolean noColor, a level of "
                       "debug, info, warn or error, and start and time as 24 character toISOString times",
                       out, out_len);
}

static int fill_entry(const cJSON *argument, log_entry *entry) {
    const cJSON *no_color = cJSON_GetObjectItemCaseSensitive(argument, "noColor");
    const char *level = string_member(argument, "level");
    entry->home = string_member(argument, "home");
    entry->plugin = string_member(argument, "plugin");
    entry->start = string_member(argument, "start");
    entry->time = string_member(argument, "time");
    entry->message = string_member(argument, "message");
    entry->names = optional_object(argument, "names");
    entry->config = optional_object(argument, "config");
    entry->level = level == NULL ? LEVEL_UNKNOWN : level_named(level);
    entry->no_color = cJSON_IsTrue(no_color);
    return entry->home != NULL && entry->plugin != NULL && entry->message != NULL && cJSON_IsBool(no_color) &&
           entry->level != LEVEL_UNKNOWN && entry->start != NULL && is_iso_time(entry->start) &&
           entry->time != NULL && is_iso_time(entry->time);
}

static char *log_file_name(const char *name, const char *start) {
    size_t size = strlen(name) + ISO_LENGTH + sizeof("-.log");
    char *file_name = malloc(size);
    if (file_name == NULL) {
        return NULL;
    }
    int written = snprintf(file_name, size, "%s-%.19s.log", name, start);
    for (int index = 0; index < written; index++) {
        if (file_name[index] == ':') {
            file_name[index] = '-';
        }
    }
    return file_name;
}

static char *log_directory(const char *home, const char *time) {
    char date[11];
    memcpy(date, time, 10);
    date[10] = '\0';
    const char *segments[] = {home, "logs", date};
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 3);
}

static char *log_line(const log_entry *entry) {
    size_t size = strlen(entry->message) + ISO_LENGTH + 16;
    char *line = malloc(size);
    if (line != NULL) {
        snprintf(line, size, "[%s] [%s] %s\n", entry->time, LEVEL_TAGS[entry->level], entry->message);
    }
    return line;
}

static int append_line(const log_entry *entry, const char *name) {
    char *directory = log_directory(entry->home, entry->time);
    char *file_name = log_file_name(name, entry->start);
    char *line = log_line(entry);
    char *path = NULL;
    int appended = 0;
    if (directory != NULL && file_name != NULL && line != NULL) {
        const char *segments[] = {directory, file_name};
        path = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
        appended = path != NULL && spisor_make_directories(directory) &&
                   spisor_append_file(path, (const uint8_t *)line, strlen(line));
    }
    free(directory);
    free(file_name);
    free(line);
    free(path);
    return appended;
}

static int is_logging_disabled(const cJSON *merged) {
    return cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(merged, "logging"));
}

/** Unreadable settings leave console logging off and colour on, as basekit's readJson fallback did. */
static console_style read_console_style(const log_entry *entry) {
    console_style style = {0, 1};
    int failed;
    cJSON *settings = runtime_read_settings(entry->home, entry->names, "settings", NULL, &failed);
    if (settings != NULL) {
        style.enabled = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(settings, "logConsole"));
        style.colored = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings, "logColor"));
        cJSON_Delete(settings);
    }
    return style;
}

static const char *level_prefix(log_level level) {
    return level == LEVEL_WARN ? "WARN " : level == LEVEL_DEBUG ? "DEBUG " : "";
}

static char *console_line(const log_entry *entry, const char *name, int colored) {
    const char *prefix = level_prefix(entry->level);
    size_t size = strlen(name) + strlen(prefix) + strlen(entry->message) + 64;
    char *line = malloc(size);
    if (line == NULL) {
        return NULL;
    }
    if (!colored) {
        snprintf(line, size, "[%s] %s%s", name, prefix, entry->message);
    } else if (entry->level == LEVEL_ERROR) {
        snprintf(line, size, "\x1b[%um[%s]" ANSI_RESET " \x1b[%dm%s" ANSI_RESET, (unsigned)runtime_name_colour(name),
                 name, ANSI_RED, entry->message);
    } else {
        snprintf(line, size, "\x1b[%um[%s]" ANSI_RESET " %s%s", (unsigned)runtime_name_colour(name), name, prefix,
                 entry->message);
    }
    return line;
}

static int32_t answer_console(char *line, uint8_t **out, size_t *out_len) {
    cJSON *result = cJSON_CreateObject();
    int added = result != NULL && (line == NULL || cJSON_AddStringToObject(result, "console", line) != NULL);
    free(line);
    int32_t status = added ? spisor_succeed(result, out, out_len) : SPISOR_STATUS_OUT_OF_MEMORY;
    cJSON_Delete(result);
    if (!added) {
        *out = NULL;
        *out_len = 0;
    }
    return status;
}

static int32_t refuse_name(uint8_t **out, size_t *out_len) {
    return spisor_fail("the plugin's config name is not a usable file name",
                       "name the plugin, or set config.name, to one portable path segment such as \"my-plugin\"", out,
                       out_len);
}

static int32_t refuse_append(uint8_t **out, size_t *out_len) {
    return spisor_fail("the log line could not be appended",
                       "check that the home's logs directory can be created and written", out, out_len);
}

static int should_print(const log_entry *entry, const console_style *style) {
    return entry->level == LEVEL_ERROR || style->enabled;
}

static int32_t write_entry(const log_entry *entry, const char *name, uint8_t **out, size_t *out_len) {
    int failed;
    cJSON *merged = runtime_read_settings(entry->home, entry->names, entry->plugin, entry->config, &failed);
    if (failed) {
        *out = NULL;
        *out_len = 0;
        return SPISOR_STATUS_OUT_OF_MEMORY;
    }
    int skip_file = is_logging_disabled(merged);
    cJSON_Delete(merged);
    if (!skip_file && !append_line(entry, name)) {
        return refuse_append(out, out_len);
    }
    console_style style = read_console_style(entry);
    char *line = should_print(entry, &style) ? console_line(entry, name, style.colored && !entry->no_color) : NULL;
    return answer_console(line, out, out_len);
}

static int32_t log_call(const spisor_call *call, uint8_t **out, size_t *out_len) {
    log_entry entry;
    if (!fill_entry(call->argument, &entry)) {
        return refuse_argument(out, out_len);
    }
    char *name = runtime_config_name(entry.plugin, entry.config);
    if (name == NULL) {
        return refuse_name(out, out_len);
    }
    int32_t status = write_entry(&entry, name, out, out_len);
    free(name);
    return status;
}

RUNTIME_EXPORT int32_t runtime_log_write(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    spisor_call call;
    int32_t status = spisor_parse_call(in, in_len, 0, &call, out, out_len);
    if (status != SPISOR_STATUS_SUCCESS) {
        return status;
    }
    status = log_call(&call, out, out_len);
    spisor_discard_call(&call);
    return status;
}
