#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#define CURRENT_PROCESS_ID _getpid()
#else
#include <unistd.h>
#define CURRENT_PROCESS_ID getpid()
#endif

#include "check.h"
#include "log.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"

#define START "2026-10-27T19:00:00.000Z"
#define TIME "2026-10-27T19:34:53.123Z"
#define LOG_FILE "/logs/2026-10-27/p-2026-10-27T19-00-00.log"

typedef struct scratch_home {
    char path[1024];
    char config_directory[1100];
} scratch_home;

typedef struct answer {
    int32_t status;
    cJSON *tree;
    uint8_t *buffer;
} answer;

static void fresh_home(scratch_home *home, const char *label) {
    static int sequence = 0;
    snprintf(home->path, sizeof home->path, RUNTIME_SCRATCH_PATH "/log-%d-%d-%s", (int)CURRENT_PROCESS_ID, sequence++,
             label);
    snprintf(home->config_directory, sizeof home->config_directory, "%s/config", home->path);
    CHECK(spisor_make_directories(home->path) == 1);
}

static void write_text(const char *path, const char *text) {
    CHECK(spisor_replace_file(path, (const uint8_t *)text, strlen(text)) == 1);
}

static void write_settings(const scratch_home *home, const char *directory_name, const char *text) {
    char directory[1200];
    char path[1300];
    snprintf(directory, sizeof directory, "%s/%s", home->path, directory_name);
    CHECK(spisor_make_directories(directory) == 1);
    snprintf(path, sizeof path, "%s/settings.json", directory);
    write_text(path, text);
}

static char *read_log(const scratch_home *home) {
    char path[1200];
    size_t length = 0;
    snprintf(path, sizeof path, "%s" LOG_FILE, home->path);
    return (char *)spisor_read_file(path, &length);
}

static cJSON *request_for(const scratch_home *home, const char *level, const char *config_json, const char *names_json,
                          int no_color) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "home", home->path);
    cJSON_AddStringToObject(request, "plugin", "p");
    cJSON_AddStringToObject(request, "start", START);
    cJSON_AddStringToObject(request, "time", TIME);
    cJSON_AddStringToObject(request, "level", level);
    cJSON_AddStringToObject(request, "message", "m");
    cJSON_AddBoolToObject(request, "noColor", no_color);
    if (config_json != NULL) {
        cJSON_AddItemToObject(request, "config", cJSON_Parse(config_json));
    }
    if (names_json != NULL) {
        cJSON_AddItemToObject(request, "names", cJSON_Parse(names_json));
    }
    return request;
}

static answer write_log(cJSON *request) {
    answer result = {0, NULL, NULL};
    char *text = cJSON_PrintUnformatted(request);
    size_t out_len = 0;
    result.status = runtime_log_write((const uint8_t *)text, strlen(text), &result.buffer, &out_len);
    result.tree = result.buffer == NULL ? NULL : spisor_parse_json(result.buffer, out_len);
    free(text);
    cJSON_Delete(request);
    return result;
}

static void finish(answer *result) {
    cJSON_Delete(result->tree);
    spisor_release(result->buffer);
}

static void expect_console(const answer *result, const char *expected, const char *label) {
    CHECK(result->status == SPISOR_STATUS_SUCCESS);
    const cJSON *console = cJSON_GetObjectItemCaseSensitive(result->tree, "console");
    if (expected == NULL) {
        if (console != NULL) {
            fprintf(stderr, "%s: expected no console line\n", label);
            check_failures++;
        }
        return;
    }
    check_text(cJSON_IsString(console) ? console->valuestring : NULL, expected, label);
}

static void expect_refused(const answer *result, const char *label) {
    if (result->status != SPISOR_STATUS_FAILURE) {
        fprintf(stderr, "%s: expected a refusal, got status %d\n", label, (int)result->status);
        check_failures++;
    }
}

static void writes_tagged_line(void) {
    scratch_home home;
    fresh_home(&home, "tagged");
    answer result = write_log(request_for(&home, "warn", NULL, NULL, 0));
    expect_console(&result, NULL, "console is off by default");
    finish(&result);
    char *content = read_log(&home);
    check_text(content, "[" TIME "] [WARN] m\n", "tagged line");
    free(content);
}

static void appends_each_level_tag(void) {
    scratch_home home;
    fresh_home(&home, "levels");
    const char *levels[] = {"debug", "info", "warn", "error"};
    for (int index = 0; index < 4; index++) {
        answer result = write_log(request_for(&home, levels[index], NULL, NULL, 0));
        finish(&result);
    }
    char *content = read_log(&home);
    check_text(content,
               "[" TIME "] [DEBUG] m\n[" TIME "] [INFO] m\n[" TIME "] [WARN] m\n[" TIME "] [ERROR] m\n",
               "four tags");
    free(content);
}

static void logging_false_from_manifest_default(void) {
    scratch_home home;
    fresh_home(&home, "gated");
    const char *config = "{\"defaults\":{\"logging\":false}}";
    answer info = write_log(request_for(&home, "info", config, NULL, 0));
    expect_console(&info, NULL, "gated info");
    finish(&info);
    answer error = write_log(request_for(&home, "error", config, NULL, 0));
    expect_console(&error, "\x1b[33m[p]\x1b[0m \x1b[31mm\x1b[0m", "gated error still prints");
    finish(&error);
    char *content = read_log(&home);
    CHECK(content == NULL);
    free(content);
}

static void console_off_by_default(void) {
    scratch_home home;
    fresh_home(&home, "off");
    answer result = write_log(request_for(&home, "info", NULL, NULL, 0));
    expect_console(&result, NULL, "info without settings");
    finish(&result);
    write_settings(&home, "config", "{\"logConsole\":\"true\"}");
    answer text = write_log(request_for(&home, "info", NULL, NULL, 0));
    expect_console(&text, NULL, "only JSON true enables");
    finish(&text);
}

static void console_on_from_settings(void) {
    scratch_home home;
    fresh_home(&home, "on");
    write_settings(&home, "config", "{\"logConsole\":true}");
    answer info = write_log(request_for(&home, "info", NULL, NULL, 0));
    expect_console(&info, "\x1b[33m[p]\x1b[0m m", "info");
    finish(&info);
    answer debug = write_log(request_for(&home, "debug", NULL, NULL, 0));
    expect_console(&debug, "\x1b[33m[p]\x1b[0m DEBUG m", "debug");
    finish(&debug);
    answer warn = write_log(request_for(&home, "warn", NULL, NULL, 0));
    expect_console(&warn, "\x1b[33m[p]\x1b[0m WARN m", "warn");
    finish(&warn);
}

static void settings_follow_renamed_directory(void) {
    scratch_home home;
    fresh_home(&home, "renamed");
    write_settings(&home, "cfg", "{\"logConsole\":true}");
    answer renamed = write_log(request_for(&home, "info", NULL, "{\"config\":\"cfg\"}", 0));
    expect_console(&renamed, "\x1b[33m[p]\x1b[0m m", "cfg/settings.json");
    finish(&renamed);
    answer default_directory = write_log(request_for(&home, "info", NULL, NULL, 0));
    expect_console(&default_directory, NULL, "config/ is not read when renamed");
    finish(&default_directory);
    char *content = read_log(&home);
    CHECK(content != NULL);
    free(content);
}

static void no_colour(void) {
    scratch_home home;
    fresh_home(&home, "plain");
    write_settings(&home, "config", "{\"logConsole\":true}");
    answer flagged = write_log(request_for(&home, "info", NULL, NULL, 1));
    expect_console(&flagged, "[p] m", "noColor");
    finish(&flagged);
    write_settings(&home, "config", "{\"logConsole\":true,\"logColor\":false}");
    answer setting = write_log(request_for(&home, "info", NULL, NULL, 0));
    expect_console(&setting, "[p] m", "logColor false");
    finish(&setting);
    answer error = write_log(request_for(&home, "error", NULL, NULL, 0));
    expect_console(&error, "[p] m", "uncoloured error");
    finish(&error);
    write_settings(&home, "config", "{\"logConsole\":true,\"logColor\":0}");
    answer zero = write_log(request_for(&home, "info", NULL, NULL, 0));
    expect_console(&zero, "\x1b[33m[p]\x1b[0m m", "only JSON false disables colour");
    finish(&zero);
}

static void error_is_red(void) {
    scratch_home home;
    fresh_home(&home, "red");
    answer result = write_log(request_for(&home, "error", NULL, NULL, 0));
    expect_console(&result, "\x1b[33m[p]\x1b[0m \x1b[31mm\x1b[0m", "error");
    finish(&result);
}

static void unreadable_settings_mean_console_off_colour_on(void) {
    scratch_home home;
    fresh_home(&home, "broken");
    write_settings(&home, "config", "{\"logConsole\":");
    answer info = write_log(request_for(&home, "info", NULL, NULL, 0));
    expect_console(&info, NULL, "broken settings, info");
    finish(&info);
    answer error = write_log(request_for(&home, "error", NULL, NULL, 0));
    expect_console(&error, "\x1b[33m[p]\x1b[0m \x1b[31mm\x1b[0m", "broken settings, error");
    finish(&error);
}

/**
 * @implNote The expected codes are basekit's prefixColor (archived/basekit/src/log.ts) run in node
 * over the same four names.
 */
static void colour_matches_basekit(void) {
    CHECK(runtime_name_colour("p") == 33);
    CHECK(runtime_name_colour("antigravity") == 95);
    CHECK(runtime_name_colour("caf\xc3\xa9") == 32);
    CHECK(runtime_name_colour("x\xf0\x9f\x98\x80y") == 36);
}

static void expect_bad_stamp(const scratch_home *home, const char *member, const char *stamp) {
    cJSON *request = request_for(home, "info", NULL, NULL, 0);
    cJSON_ReplaceItemInObjectCaseSensitive(request, member, cJSON_CreateString(stamp));
    answer result = write_log(request);
    expect_refused(&result, stamp);
    finish(&result);
}

static void refuses_malformed_arguments(void) {
    scratch_home home;
    fresh_home(&home, "refused");
    const char *levels[] = {"trace", "INFO", ""};
    for (int index = 0; index < 3; index++) {
        answer result = write_log(request_for(&home, levels[index], NULL, NULL, 0));
        expect_refused(&result, levels[index]);
        finish(&result);
    }
    const char *stamps[] = {"2026-10-27T19:34:53Z", "2026-10-27 19:34:53.123Z", "2026-10-27T19:34:53.123X",
                            "26-10-27T19:34:53.1234Z"};
    for (int index = 0; index < 4; index++) {
        expect_bad_stamp(&home, "time", stamps[index]);
        expect_bad_stamp(&home, "start", stamps[index]);
    }
    cJSON *missing = request_for(&home, "info", NULL, NULL, 0);
    cJSON_DeleteItemFromObjectCaseSensitive(missing, "message");
    answer no_message = write_log(missing);
    expect_refused(&no_message, "no message");
    finish(&no_message);
    char *content = read_log(&home);
    CHECK(content == NULL);
    free(content);
}

static void block_logs_directory(const scratch_home *home) {
    char blocker[1200];
    snprintf(blocker, sizeof blocker, "%s/logs", home->path);
    write_text(blocker, "a file where the directory belongs");
}

static void refuses_failed_append_without_console_line(void) {
    scratch_home home;
    fresh_home(&home, "blocked");
    block_logs_directory(&home);
    answer result = write_log(request_for(&home, "info", NULL, NULL, 0));
    expect_refused(&result, "unwritable log, console off");
    finish(&result);
}

static void failed_append_keeps_due_console_line(void) {
    scratch_home home;
    fresh_home(&home, "blocked-console");
    block_logs_directory(&home);
    answer error = write_log(request_for(&home, "error", NULL, NULL, 0));
    expect_console(&error, "\x1b[33m[p]\x1b[0m \x1b[31mm\x1b[0m", "unwritable log, error");
    finish(&error);
    write_settings(&home, "config", "{\"logConsole\":true}");
    answer info = write_log(request_for(&home, "info", NULL, NULL, 0));
    expect_console(&info, "\x1b[33m[p]\x1b[0m m", "unwritable log, console on");
    finish(&info);
}

static void refuses_unusable_home(void) {
    const char *homes[] = {"", "log-test-relative-home"};
    const char *levels[] = {"info", "error"};
    for (int home_index = 0; home_index < 2; home_index++) {
        for (int level_index = 0; level_index < 2; level_index++) {
            scratch_home home;
            snprintf(home.path, sizeof home.path, "%s", homes[home_index]);
            answer result = write_log(request_for(&home, levels[level_index], NULL, NULL, 0));
            expect_refused(&result, homes[home_index]);
            CHECK(cJSON_GetObjectItemCaseSensitive(result.tree, "fix") != NULL);
            finish(&result);
        }
    }
    CHECK(spisor_file_exists("log-test-relative-home") == 0);
    CHECK(spisor_file_exists("logs") == 0);
}

int main(void) {
    writes_tagged_line();
    appends_each_level_tag();
    logging_false_from_manifest_default();
    console_off_by_default();
    console_on_from_settings();
    settings_follow_renamed_directory();
    no_colour();
    error_is_red();
    unreadable_settings_mean_console_off_colour_on();
    colour_matches_basekit();
    refuses_malformed_arguments();
    refuses_failed_append_without_console_line();
    failed_append_keeps_due_console_line();
    refuses_unusable_home();
    return check_summary("log_test");
}
