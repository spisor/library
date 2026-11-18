#include <stddef.h>
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
#include "spisor/allocation.h"
#include "scratch_directory.h"
#include "log.h"
#include "runtime/log.h"
#include "spisor/file.h"
#include "vector_reader.h"

#define START "2026-10-27T19:00:00.000Z"
#define TIME "2026-10-27T19:34:53.123Z"
#define LOG_FILE "/logs/2026-10-27/p-2026-10-27T19-00-00.log"

typedef struct scratch_home {
    char path[1024];
    char config_directory[1100];
    char portable[1100];
} scratch_home;

/** What a write answered: its status, and its answer as JSON, {"console"?} or the refusal's {"message", "fix"}. */
typedef struct answer {
    spisor_status status;
    cJSON *tree;
} answer;

static void fresh_home(scratch_home *home, const char *label) {
    static int sequence = 0;
    snprintf(home->path, sizeof home->path, RUNTIME_SCRATCH_PATH "/log-%d-%d-%s", (int)CURRENT_PROCESS_ID, sequence++,
             label);
    snprintf(home->config_directory, sizeof home->config_directory, "%s/config", home->path);
    snprintf(home->portable, sizeof home->portable, "%s/portable", home->path);
    start_empty_directory(home->path);
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

static void write_settings_file(const scratch_home *home, const char *name, const char *text) {
    char path[1200];
    CHECK(spisor_make_directories(home->config_directory) == 1);
    snprintf(path, sizeof path, "%s/%s.json", home->config_directory, name);
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

static const char *text_of(const cJSON *request, const char *member) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, member));
}

/** A level's runtime_log_level, or one no level has for a name the service does not know. */
static uint32_t level_of(const char *name) {
    const char *const names[] = {"debug", "info", "warn", "error"};
    for (uint32_t index = 0; name != NULL && index < 4; index++) {
        if (strcmp(name, names[index]) == 0) {
            return index;
        }
    }
    return 99;
}

static const spisor_value *value_of(const cJSON *request, const char *member, spisor_value_tree *tree) {
    if (cJSON_GetObjectItemCaseSensitive(request, member) == NULL || !spisor_vector_value(request, member, tree)) {
        return NULL;
    }
    return &tree->root;
}

static cJSON *answer_tree(spisor_status status, const runtime_log_answer *written, const spisor_refusal *refusal) {
    cJSON *tree = cJSON_CreateObject();
    if (status == SPISOR_OK && written->console != NULL) {
        cJSON_AddStringToObject(tree, "console", written->console);
    }
    if (refusal->detail != NULL) {
        cJSON_AddStringToObject(tree, "message", refusal->detail);
    }
    if (refusal->fix != NULL) {
        cJSON_AddStringToObject(tree, "fix", refusal->fix);
    }
    return tree;
}

/** Writes the line a request written as JSON test data describes, deleting the request. */
static answer write_log(cJSON *request) {
    const runtime_log_definition *log = runtime_log_definition_of();
    runtime_log_entry entry = SPISOR_INIT(runtime_log_entry);
    runtime_log_answer written = SPISOR_INIT(runtime_log_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_value_tree names = SPISOR_INIT(spisor_value_tree);
    spisor_value_tree config = SPISOR_INIT(spisor_value_tree);
    answer result;
    entry.home = text_of(request, "home");
    entry.portable = text_of(request, "portable");
    entry.plugin = text_of(request, "plugin");
    entry.start = text_of(request, "start");
    entry.time = text_of(request, "time");
    entry.message = text_of(request, "message");
    entry.level = level_of(text_of(request, "level"));
    entry.no_color = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "noColor"));
    entry.names = value_of(request, "names", &names);
    entry.config = value_of(request, "config", &config);
    result.status = log->write(log->context, &entry, &written, &refusal);
    result.tree = answer_tree(result.status, &written, &refusal);
    log->release_answer(log->context, &written);
    log->release_refusal(log->context, &refusal);
    CHECK(written.storage == NULL && refusal.storage == NULL);
    spisor_value_release(&names);
    spisor_value_release(&config);
    cJSON_Delete(request);
    return result;
}

static void finish(answer *result) {
    cJSON_Delete(result->tree);
}

static void expect_console(const answer *result, const char *expected, const char *label) {
    CHECK(result->status == SPISOR_OK);
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
    if (result->status != SPISOR_MALFORMED && result->status != SPISOR_REFUSED) {
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
    spisor_memory_release(content);
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
    spisor_memory_release(content);
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
    spisor_memory_release(content);
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
    spisor_memory_release(content);
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
    spisor_memory_release(content);
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

static cJSON *with_portable(cJSON *request, const scratch_home *home) {
    cJSON_AddStringToObject(request, "portable", home->portable);
    return request;
}

static void write_portable(const scratch_home *home, const char *relative_directory, const char *file_name,
                           const char *text) {
    char directory[1300];
    char path[1400];
    snprintf(directory, sizeof directory, "%s%s", home->portable, relative_directory);
    CHECK(spisor_make_directories(directory) == 1);
    snprintf(path, sizeof path, "%s/%s", directory, file_name);
    write_text(path, text);
}

static void console_gate_from_portable_settings(void) {
    scratch_home home;
    fresh_home(&home, "portable-console");
    write_portable(&home, "", "settings.json", "{\"logConsole\":true}");
    answer shared = write_log(with_portable(request_for(&home, "info", NULL, NULL, 0), &home));
    expect_console(&shared, "\x1b[33m[p]\x1b[0m m", "logConsole from the portable settings");
    finish(&shared);
    answer unlayered = write_log(request_for(&home, "info", NULL, NULL, 0));
    expect_console(&unlayered, NULL, "no portable folder given");
    finish(&unlayered);
    write_settings(&home, "config", "{\"logConsole\":false}");
    answer overridden = write_log(with_portable(request_for(&home, "info", NULL, NULL, 0), &home));
    expect_console(&overridden, NULL, "the application's settings override the portable");
    finish(&overridden);
}

static void colour_from_portable_settings(void) {
    scratch_home home;
    fresh_home(&home, "portable-colour");
    write_portable(&home, "", "settings.json", "{\"logConsole\":true,\"logColor\":false}");
    write_settings(&home, "config", "{\"other\":1}");
    answer plain = write_log(with_portable(request_for(&home, "info", NULL, NULL, 0), &home));
    expect_console(&plain, "[p] m", "logColor from the portable settings under an unrelated application file");
    finish(&plain);
}

static void logging_gate_from_portable_plugin_file(void) {
    scratch_home home;
    fresh_home(&home, "portable-gate");
    write_portable(&home, "/plugins/p", "config.json", "{\"logging\":false}");
    answer gated = write_log(with_portable(request_for(&home, "info", NULL, NULL, 0), &home));
    finish(&gated);
    char *content = read_log(&home);
    CHECK(content == NULL);
    spisor_memory_release(content);
    write_settings_file(&home, "p", "{\"logging\":true}");
    answer reopened = write_log(with_portable(request_for(&home, "info", NULL, NULL, 0), &home));
    finish(&reopened);
    content = read_log(&home);
    check_text(content, "[" TIME "] [INFO] m\n", "the application file reopens the log");
    spisor_memory_release(content);
}

static void absent_portable_folder_changes_no_log(void) {
    scratch_home home;
    fresh_home(&home, "portable-absent");
    write_settings(&home, "config", "{\"logConsole\":true}");
    answer result = write_log(with_portable(request_for(&home, "info", NULL, NULL, 0), &home));
    expect_console(&result, "\x1b[33m[p]\x1b[0m m", "absent portable folder");
    finish(&result);
    CHECK(spisor_file_exists(home.portable) == 0);
}

static void refuses_unusable_portable(void) {
    scratch_home home;
    fresh_home(&home, "portable-relative");
    cJSON *request = request_for(&home, "info", NULL, NULL, 0);
    cJSON_AddStringToObject(request, "portable", "relative-portable");
    answer result = write_log(request);
    expect_refused(&result, "relative portable");
    finish(&result);
    cJSON *escaping = with_portable(request_for(&home, "info", "{\"name\":\"p\"}", NULL, 0), &home);
    cJSON_ReplaceItemInObjectCaseSensitive(escaping, "plugin", cJSON_CreateString("../p"));
    answer bad_id = write_log(escaping);
    expect_refused(&bad_id, "an id that is no portable folder name");
    finish(&bad_id);
    char *content = read_log(&home);
    CHECK(content == NULL);
    spisor_memory_release(content);
}

static void the_definition_is_one_static_struct(void) {
    const runtime_log_definition *log = runtime_log_definition_of();
    CHECK(log != NULL && (const void *)log == runtime_log_definition_of());
    CHECK(log != NULL && log->size == sizeof(runtime_log_definition));
}

static void refuses_an_entry_or_answer_short_of_its_shape_and_names_that_are_no_record(void) {
    const runtime_log_definition *log = runtime_log_definition_of();
    scratch_home home;
    runtime_log_entry entry = SPISOR_INIT(runtime_log_entry);
    runtime_log_answer written = SPISOR_INIT(runtime_log_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_value_tree names = SPISOR_INIT(spisor_value_tree);
    cJSON *listed = cJSON_Parse("{\"names\": [\"logs\"]}");
    fresh_home(&home, "typed");
    entry.home = home.path;
    entry.plugin = "p";
    entry.start = START;
    entry.time = TIME;
    entry.message = "m";
    entry.level = RUNTIME_LOG_ERROR;
    entry.size = (uint32_t)offsetof(runtime_log_entry, no_color);
    CHECK(log->write(log->context, &entry, &written, &refusal) == SPISOR_MALFORMED);
    log->release_refusal(log->context, &refusal);
    entry.size = sizeof entry;
    written.size = (uint32_t)offsetof(runtime_log_answer, storage);
    CHECK(log->write(log->context, &entry, &written, &refusal) == SPISOR_MALFORMED);
    log->release_refusal(log->context, &refusal);
    written.size = sizeof written;
    CHECK(spisor_vector_value(listed, "names", &names));
    entry.names = &names.root;
    CHECK(log->write(log->context, &entry, &written, &refusal) == SPISOR_MALFORMED);
    CHECK(refusal.detail != NULL && strstr(refusal.detail, "names") != NULL);
    log->release_refusal(log->context, &refusal);
    spisor_value_release(&names);
    cJSON_Delete(listed);
    char *content = read_log(&home);
    CHECK(content == NULL);
    spisor_memory_release(content);
}

int main(void) {
    the_definition_is_one_static_struct();
    refuses_an_entry_or_answer_short_of_its_shape_and_names_that_are_no_record();
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
    console_gate_from_portable_settings();
    colour_from_portable_settings();
    logging_gate_from_portable_plugin_file();
    absent_portable_folder_changes_no_log();
    refuses_unusable_portable();
    return check_summary("log_test");
}
