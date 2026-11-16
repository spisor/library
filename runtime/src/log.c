#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "definition_support.h"
#include "paths.h"
#include "segment.h"
#include "spisor/file.h"
#include "spisor/path.h"

#define ISO_LENGTH 24
#define ANSI_RESET "\x1b[0m"
#define ANSI_RED 31

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

static int is_digits(const char *text, size_t count) {
    for (size_t index = 0; index < count; index++) {
        if (text[index] < '0' || text[index] > '9') {
            return 0;
        }
    }
    return 1;
}

static int is_iso_time(const char *text) {
    if (text == NULL) {
        return 0;
    }
    return strlen(text) == ISO_LENGTH && is_digits(text, 4) && text[4] == '-' && is_digits(text + 5, 2) &&
           text[7] == '-' && is_digits(text + 8, 2) && text[10] == 'T' && is_digits(text + 11, 2) &&
           text[13] == ':' && is_digits(text + 14, 2) && text[16] == ':' && is_digits(text + 17, 2) &&
           text[19] == '.' && is_digits(text + 20, 3) && text[23] == 'Z';
}

static spisor_status refuse_entry(spisor_refusal *refusal) {
    return runtime_refuse_as(refusal, SPISOR_MALFORMED, "the argument is not a well formed log entry",
                             "send home, plugin, start, time and message as text, level as a runtime_log_level, and "
                             "start and time as 24 character toISOString times");
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

static char *log_line(const runtime_log_request *entry) {
    size_t size = strlen(entry->message) + ISO_LENGTH + 16;
    char *line = malloc(size);
    if (line != NULL) {
        snprintf(line, size, "[%s] [%s] %s\n", entry->time, LEVEL_TAGS[entry->level], entry->message);
    }
    return line;
}

static int append_line(const runtime_log_request *entry, const char *name) {
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

/**
 * The console style the ecosystem settings choose; 0 when memory ran out. An unreadable settings file contributes
 * nothing, so it leaves console logging off and colour on, as the TypeScript read-json fallback did.
 */
static int read_console_style(const runtime_log_request *entry, console_style *style) {
    int failed;
    cJSON *settings = runtime_read_ecosystem_settings(entry->home, entry->names, entry->portable, &failed);
    style->enabled = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(settings, "logConsole"));
    style->colored = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings, "logColor"));
    cJSON_Delete(settings);
    return !failed;
}

static const char *level_prefix(uint32_t level) {
    return level == RUNTIME_LOG_WARN ? "WARN " : level == RUNTIME_LOG_DEBUG ? "DEBUG " : "";
}

static char *console_line(const runtime_log_request *entry, const char *name, int colored) {
    const char *prefix = level_prefix(entry->level);
    size_t size = strlen(name) + strlen(prefix) + strlen(entry->message) + 64;
    char *line = malloc(size);
    if (line == NULL) {
        return NULL;
    }
    if (!colored) {
        snprintf(line, size, "[%s] %s%s", name, prefix, entry->message);
    } else if (entry->level == RUNTIME_LOG_ERROR) {
        snprintf(line, size, "\x1b[%um[%s]" ANSI_RESET " \x1b[%dm%s" ANSI_RESET, (unsigned)runtime_name_colour(name),
                 name, ANSI_RED, entry->message);
    } else {
        snprintf(line, size, "\x1b[%um[%s]" ANSI_RESET " %s%s", (unsigned)runtime_name_colour(name), name, prefix,
                 entry->message);
    }
    return line;
}

static spisor_status refuse_name(spisor_refusal *refusal) {
    return runtime_refuse_as(refusal, SPISOR_MALFORMED, "the plugin's config name is not a usable file name",
                             "name the plugin, or set config.name, to one portable path segment such as \"my-plugin\"");
}

static spisor_status refuse_id(spisor_refusal *refusal) {
    return runtime_refuse_as(refusal, SPISOR_MALFORMED,
                             "the plugin id is not a usable folder name in the portable place",
                             "name the plugin by a manifest id that is one portable path segment such as "
                             "\"my-plugin\"");
}

/** Whether the portable layer, when there is one, can name the plugin's folder by its id. */
static int is_portable_id_usable(const runtime_log_request *entry) {
    if (entry->portable == NULL) {
        return 1;
    }
    char *folder_name = runtime_portable_segment(entry->plugin);
    int usable = folder_name != NULL;
    free(folder_name);
    return usable;
}

static spisor_status refuse_append(spisor_refusal *refusal) {
    return runtime_refuse_as(refusal, SPISOR_REFUSED, "the log line could not be appended",
                             "check that the home's logs directory can be created and written");
}

static int should_print(const runtime_log_request *entry, const console_style *style) {
    return entry->level == RUNTIME_LOG_ERROR || style->enabled;
}

/** The console line due, or NULL with none due, in the block `storage` owns; 0 when memory ran out. */
static int pack_console(const runtime_log_request *entry, const char *name, const console_style *style,
                        runtime_log_console *console) {
    char *line = should_print(entry, style) ? console_line(entry, name, style->colored && !entry->no_color) : NULL;
    int packed = (line != NULL || !should_print(entry, style)) &&
                 runtime_pack_texts((const char *const *)&line, &console->line, 1, &console->storage);
    free(line);
    return packed;
}

/**
 * @implNote Everything that can run out of memory comes before the append, so a write answering OUT_OF_MEMORY never
 * appended and a retry logs the line once.
 */
static spisor_status write_entry(const runtime_log_request *entry, const char *name, runtime_log_console *console,
                                 spisor_refusal *refusal) {
    int failed;
    console_style style;
    cJSON *merged =
        runtime_read_settings(entry->home, entry->names, entry->portable, entry->plugin, entry->config, &failed);
    if (failed) {
        return SPISOR_OUT_OF_MEMORY;
    }
    int skip_file = is_logging_disabled(merged);
    cJSON_Delete(merged);
    if (!read_console_style(entry, &style) || !pack_console(entry, name, &style, console)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    if (!skip_file && !append_line(entry, name) && console->line == NULL) {
        runtime_release_storage(console->storage);
        console->storage = NULL;
        return refuse_append(refusal);
    }
    return SPISOR_OK;
}

static int is_well_formed(const runtime_log_request *entry) {
    return entry->home != NULL && entry->plugin != NULL && entry->message != NULL &&
           entry->level <= RUNTIME_LOG_ERROR && is_iso_time(entry->start) && is_iso_time(entry->time);
}

spisor_status runtime_write_log(const runtime_log_request *entry, runtime_log_console *console,
                                spisor_refusal *refusal) {
    console->line = NULL;
    console->storage = NULL;
    if (!is_well_formed(entry)) {
        return refuse_entry(refusal);
    }
    if (!runtime_is_usable_home(entry->home)) {
        return runtime_refuse_home(refusal);
    }
    if (entry->portable != NULL && !runtime_is_usable_home(entry->portable)) {
        return runtime_refuse_portable(refusal);
    }
    if (!is_portable_id_usable(entry)) {
        return refuse_id(refusal);
    }
    char *name = runtime_config_name(entry->plugin, entry->config);
    if (name == NULL) {
        return refuse_name(refusal);
    }
    spisor_status status = write_entry(entry, name, console, refusal);
    free(name);
    return status;
}
