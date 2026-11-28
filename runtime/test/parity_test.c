#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "cJSON.h"
#include "check.h"
#include "spisor/json_allocation.h"
#include "parity_files.h"
#include "parity_json.h"
#include "runtime/config.h"
#include "runtime/log.h"
#include "runtime/paths.h"
#include "spisor/file.h"
#include "spisor/status.h"
#include "spisor/value.h"
#include "user_home.h"
#include "vector_reader.h"

/**
 * @file
 * Parity's observer: lays out every case under parity/cases, makes its calls through the built shared runtime's
 * definitions as a host would, writes each answer and the home's log and config files in the recorded form, and
 * compares them with parity/expected, naming the case and the first path at which they differ.
 */

#define DEFAULT_START "2026-10-27T19:34:53.123Z"

#ifdef _WIN32
typedef HMODULE library_handle;

static library_handle load_runtime(void) {
    return LoadLibraryA(RUNTIME_LIBRARY_PATH);
}

static const void *definition_of(library_handle library, const char *symbol) {
    FARPROC getter = GetProcAddress(library, symbol);
    return getter == NULL ? NULL : ((const void *(*)(void))(void (*)(void))getter)();
}

static void unload_runtime(library_handle library) {
    FreeLibrary(library);
}
#else
typedef void *library_handle;

static library_handle load_runtime(void) {
    return dlopen(RUNTIME_LIBRARY_PATH, RTLD_NOW | RTLD_LOCAL);
}

static const void *definition_of(library_handle library, const char *symbol) {
    void *getter = dlsym(library, symbol);
    const void *(*call)(void) = NULL;
    if (getter == NULL) {
        return NULL;
    }
    memcpy(&call, &getter, sizeof call);
    return call();
}

static void unload_runtime(library_handle library) {
    dlclose(library);
}
#endif

typedef struct runtime_services {
    const runtime_paths_definition *paths;
    const runtime_config_definition *config;
    const runtime_log_definition *log;
} runtime_services;

typedef struct staged_case {
    const char *name;
    cJSON *definition;
    char *root;
    char *home;
    char *portable;
    const char *plugin;
    spisor_value_tree names;
    spisor_value_tree config;
    const char *start;
    const cJSON *times;
    uint32_t no_color;
    int log_index;
} staged_case;

static const char *text_member(const cJSON *object, const char *key) {
    const cJSON *member = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(member) ? member->valuestring : NULL;
}

static int optional_value(const cJSON *object, const char *key, spisor_value_tree *tree) {
    return cJSON_GetObjectItemCaseSensitive(object, key) == NULL || spisor_vector_value(object, key, tree);
}

static const spisor_value *root_or_null(const spisor_value_tree *tree) {
    return tree->root.kind == SPISOR_VALUE_NONE ? NULL : &tree->root;
}

static char *case_file(const char *name, const char *file) {
    char *cases = parity_join(PARITY_PATH, "cases");
    char *folder = cases == NULL ? NULL : parity_join(cases, name);
    char *path = folder == NULL ? NULL : parity_join(folder, file);
    free(folder);
    free(cases);
    return path;
}

static int copy_home_fixtures(const staged_case *staged) {
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(staged->definition, "home");
    const cJSON *file = NULL;
    cJSON_ArrayForEach(file, files) {
        char *relative = parity_join("home", file->valuestring);
        char *source = relative == NULL ? NULL : case_file(staged->name, relative);
        char *target = parity_join(staged->home, file->valuestring);
        int copied = source != NULL && target != NULL && parity_copy_file(source, target);
        free(target);
        free(source);
        free(relative);
        if (!copied) {
            fprintf(stderr, "%s: could not lay out home/%s\n", staged->name, file->valuestring);
            return 0;
        }
    }
    return 1;
}

static int read_case_facts(staged_case *staged) {
    const cJSON *manifest = cJSON_GetObjectItemCaseSensitive(staged->definition, "manifest");
    const cJSON *clock = cJSON_GetObjectItemCaseSensitive(staged->definition, "clock");
    const cJSON *environment = cJSON_GetObjectItemCaseSensitive(staged->definition, "environment");
    const char *portable = text_member(staged->definition, "portable");
    staged->plugin = text_member(manifest, "id");
    staged->start = clock == NULL ? DEFAULT_START : text_member(clock, "start");
    staged->times = cJSON_GetObjectItemCaseSensitive(clock, "times");
    staged->no_color = runtime_environment_asks_no_color(environment) ? 1u : 0u;
    staged->portable = portable == NULL ? NULL : parity_join(staged->root, portable);
    return staged->plugin != NULL && staged->start != NULL && (portable == NULL || staged->portable != NULL) &&
           optional_value(staged->definition, "names", &staged->names) &&
           optional_value(manifest, "config", &staged->config);
}

/** Lays one case out afresh under the work folder, its fixture files copied byte for byte into the home. */
static int stage_case(const char *name, staged_case *staged) {
    char *definition_path = case_file(name, "case.json");
    staged->name = name;
    staged->definition = definition_path == NULL ? NULL : spisor_vector_read(definition_path);
    staged->root = parity_join(PARITY_WORK_PATH, name);
    staged->home = staged->root == NULL ? NULL : parity_join(staged->root, "home");
    free(definition_path);
    if (staged->definition == NULL || staged->home == NULL || !parity_remove_tree(staged->root) ||
        spisor_make_directories(staged->home) != SPISOR_OK) {
        fprintf(stderr, "%s: could not read the case or lay out its home\n", name);
        return 0;
    }
    return copy_home_fixtures(staged) && read_case_facts(staged);
}

static void release_staged(staged_case *staged) {
    spisor_value_release(&staged->names);
    spisor_value_release(&staged->config);
    cJSON_Delete(staged->definition);
    free(staged->portable);
    free(staged->home);
    free(staged->root);
}

static cJSON *failure_answer(spisor_status status, const spisor_refusal *refusal) {
    cJSON *answer = cJSON_CreateObject();
    cJSON *failure = cJSON_AddObjectToObject(answer, "failure");
    if (failure != NULL && refusal->detail != NULL) {
        cJSON_AddStringToObject(failure, "message", refusal->detail);
    }
    if (failure != NULL && refusal->fix != NULL) {
        cJSON_AddStringToObject(failure, "fix", refusal->fix);
    }
    if (failure != NULL && refusal->detail == NULL && refusal->fix == NULL) {
        cJSON_AddNumberToObject(failure, "status", (double)status);
    }
    return answer;
}

static int add_text_if_set(cJSON *object, const char *key, const char *text) {
    return text == NULL || cJSON_AddStringToObject(object, key, text) != NULL;
}

static int add_value_if_set(cJSON *object, const char *key, const spisor_value *value) {
    return value->kind == SPISOR_VALUE_NONE || cJSON_AddItemToObject(object, key, parity_json_of_value(value));
}

static cJSON *paths_answer_json(const runtime_paths_answer *answer) {
    cJSON *json = cJSON_CreateObject();
    int filled = json != NULL && add_text_if_set(json, "home", answer->home) &&
                 add_text_if_set(json, "repos", answer->repos) && add_text_if_set(json, "plugin", answer->plugin) &&
                 add_text_if_set(json, "cache", answer->cache) && add_text_if_set(json, "config", answer->config) &&
                 add_text_if_set(json, "portable", answer->portable) &&
                 add_text_if_set(json, "pluginCache", answer->plugin_cache);
    if (!filled) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static cJSON *resolve_paths(const runtime_services *services, const staged_case *staged) {
    const runtime_paths_definition *paths = services->paths;
    runtime_paths_resolve_argument argument = {
        .size = sizeof argument, .home = staged->home, .names = root_or_null(&staged->names), .plugin = NULL,
        .places = NULL};
    runtime_paths_answer answer = SPISOR_INIT(runtime_paths_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status = paths->resolve(paths->context, &argument, &answer, &refusal);
    cJSON *json = status == SPISOR_OK ? paths_answer_json(&answer) : failure_answer(status, &refusal);
    paths->release_answer(paths->context, &answer);
    paths->release_refusal(paths->context, &refusal);
    return json;
}

static runtime_config_plugin config_plugin_of(const staged_case *staged) {
    runtime_config_plugin plugin = {
        .size = sizeof plugin, .home = staged->home, .names = root_or_null(&staged->names),
        .portable = staged->portable, .plugin = staged->plugin, .config = root_or_null(&staged->config),
        .settings = NULL, .setting_count = 0, .places = NULL};
    return plugin;
}

static cJSON *value_answer(spisor_status status, const char *member, const spisor_value *value,
                           const spisor_refusal *refusal) {
    if (status != SPISOR_OK && status != SPISOR_NONE) {
        return failure_answer(status, refusal);
    }
    cJSON *answer = cJSON_CreateObject();
    if (answer != NULL && status == SPISOR_OK && !add_value_if_set(answer, member, value)) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static cJSON *config_all(const runtime_services *services, const staged_case *staged) {
    const runtime_config_definition *config = services->config;
    runtime_config_plugin plugin = config_plugin_of(staged);
    spisor_value_tree values = SPISOR_INIT(spisor_value_tree);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status = config->all(config->context, &plugin, &values, &refusal);
    cJSON *json = value_answer(status, "values", &values.root, &refusal);
    config->release_tree(config->context, &values);
    config->release_refusal(config->context, &refusal);
    return json;
}

static cJSON *config_get(const runtime_services *services, const staged_case *staged, const cJSON *argument) {
    const runtime_config_definition *config = services->config;
    runtime_config_plugin plugin = config_plugin_of(staged);
    runtime_config_get_argument get = {.size = sizeof get, .key = text_member(argument, "key")};
    spisor_value_tree value = SPISOR_INIT(spisor_value_tree);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status = config->get(config->context, &plugin, &get, &value, &refusal);
    cJSON *json = value_answer(status, "value", &value.root, &refusal);
    config->release_tree(config->context, &value);
    config->release_refusal(config->context, &refusal);
    return json;
}

static cJSON *config_set(const runtime_services *services, const staged_case *staged, const cJSON *argument) {
    const runtime_config_definition *config = services->config;
    spisor_value_tree value = SPISOR_INIT(spisor_value_tree);
    if (!spisor_vector_value(argument, "value", &value)) {
        return NULL;
    }
    runtime_config_plugin plugin = config_plugin_of(staged);
    runtime_settings_change change = {
        .size = sizeof change, .key = text_member(argument, "key"), .value = &value.root,
        .layer = RUNTIME_SETTINGS_LAYER_DEFAULT};
    runtime_config_set_answer answer = SPISOR_INIT(runtime_config_set_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status = config->set(config->context, &plugin, &change, &answer, &refusal);
    cJSON *json = value_answer(status, "previous", &answer.previous, &refusal);
    config->release_set_answer(config->context, &answer);
    config->release_refusal(config->context, &refusal);
    spisor_value_release(&value);
    return json;
}

static int level_of(const char *name, uint32_t *level) {
    static const char *const names[] = {"debug", "info", "warn", "error"};
    for (uint32_t index = 0; name != NULL && index < sizeof names / sizeof names[0]; index++) {
        if (strcmp(name, names[index]) == 0) {
            *level = index;
            return 1;
        }
    }
    return 0;
}

/** The time of the case's next log call: its clock's next time, or its start when the clock names no more. */
static const char *next_log_time(staged_case *staged) {
    const cJSON *time = cJSON_GetArrayItem(staged->times, staged->log_index);
    staged->log_index++;
    return cJSON_IsString(time) ? time->valuestring : staged->start;
}

static cJSON *log_write(const runtime_services *services, staged_case *staged, const cJSON *argument) {
    const runtime_log_definition *log = services->log;
    uint32_t level = RUNTIME_LOG_INFO;
    if (!level_of(text_member(argument, "level"), &level)) {
        return NULL;
    }
    runtime_log_entry entry = {
        .size = sizeof entry, .home = staged->home, .names = root_or_null(&staged->names),
        .portable = staged->portable, .plugin = staged->plugin, .config = root_or_null(&staged->config),
        .start = staged->start, .time = next_log_time(staged), .level = level,
        .message = text_member(argument, "message"), .no_color = staged->no_color};
    runtime_log_answer answer = SPISOR_INIT(runtime_log_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status = log->write(log->context, &entry, &answer, &refusal);
    cJSON *json = status == SPISOR_OK ? cJSON_CreateObject() : failure_answer(status, &refusal);
    if (json != NULL && status == SPISOR_OK && !add_text_if_set(json, "console", answer.console)) {
        cJSON_Delete(json);
        json = NULL;
    }
    log->release_answer(log->context, &answer);
    log->release_refusal(log->context, &refusal);
    return json;
}

/** The harness's own step between library calls, an edit a person makes by hand, answered `{}`. */
static cJSON *hand_edit(const staged_case *staged, const cJSON *argument) {
    const char *relative = text_member(argument, "path");
    const char *text = text_member(argument, "text");
    char *path = relative == NULL ? NULL : parity_join(staged->home, relative);
    int written = path != NULL && text != NULL && parity_write_file(path, text, strlen(text));
    free(path);
    return written ? cJSON_CreateObject() : NULL;
}

static cJSON *answer_of(const runtime_services *services, staged_case *staged, const cJSON *call) {
    const char *function = text_member(call, "function");
    const cJSON *argument = cJSON_GetObjectItemCaseSensitive(call, "argument");
    if (function == NULL) {
        return NULL;
    }
    if (strcmp(function, "file.write") == 0) {
        return hand_edit(staged, argument);
    }
    if (strcmp(function, "paths.resolve") == 0) {
        return resolve_paths(services, staged);
    }
    if (strcmp(function, "config.all") == 0) {
        return config_all(services, staged);
    }
    if (strcmp(function, "config.get") == 0) {
        return config_get(services, staged, argument);
    }
    if (strcmp(function, "config.set") == 0) {
        return config_set(services, staged, argument);
    }
    if (strcmp(function, "log.write") == 0) {
        return log_write(services, staged, argument);
    }
    fprintf(stderr, "%s: a call names %s, which the observer does not make\n", staged->name, function);
    return NULL;
}

static cJSON *answers_of(const runtime_services *services, staged_case *staged) {
    cJSON *answers = cJSON_CreateArray();
    const cJSON *call = NULL;
    cJSON_ArrayForEach(call, cJSON_GetObjectItemCaseSensitive(staged->definition, "calls")) {
        cJSON *answer = answer_of(services, staged, call);
        if (answers == NULL || !cJSON_AddItemToArray(answers, answer)) {
            cJSON_Delete(answer);
            cJSON_Delete(answers);
            return NULL;
        }
    }
    return answers;
}

/**
 * Every file under the home's logs and config folders, and under the folder a `names.config` renames config to.
 *
 * @implNote Only these are compared: basekit also wrote its bus and activity under `events/`, which the runtime never
 * writes.
 */
static cJSON *home_files_of(const staged_case *staged) {
    const char *renamed = text_member(cJSON_GetObjectItemCaseSensitive(staged->definition, "names"), "config");
    const char *folders[] = {"logs", "config", renamed};
    cJSON *files = cJSON_CreateObject();
    for (size_t index = 0; files != NULL && index < sizeof folders / sizeof folders[0]; index++) {
        int repeated = folders[index] == NULL || (index == 2 && (strcmp(renamed, "logs") == 0 ||
                                                                 strcmp(renamed, "config") == 0));
        if (!repeated && !parity_add_files_under(staged->home, folders[index], files)) {
            cJSON_Delete(files);
            files = NULL;
        }
    }
    return files;
}

static cJSON *observe_case(const runtime_services *services, staged_case *staged) {
    cJSON *observed = cJSON_CreateObject();
    cJSON *answers = answers_of(services, staged);
    cJSON *files = answers == NULL ? NULL : home_files_of(staged);
    parity_tokens tokens = {.home = staged->home, .root = staged->root};
    if (observed == NULL || answers == NULL || files == NULL || !cJSON_AddItemToObject(observed, "answers", answers)) {
        cJSON_Delete(files);
        cJSON_Delete(answers);
        cJSON_Delete(observed);
        return NULL;
    }
    if (!cJSON_AddItemToObject(observed, "files", files) || !parity_normalise(observed, &tokens)) {
        cJSON_Delete(observed);
        return NULL;
    }
    return observed;
}

static cJSON *expected_of(const char *name) {
    char *folder = parity_join(PARITY_PATH, "expected");
    size_t length = strlen(name) + sizeof ".json";
    char *file = malloc(length);
    char *path = NULL;
    if (folder != NULL && file != NULL) {
        snprintf(file, length, "%s.json", name);
        path = parity_join(folder, file);
    }
    cJSON *expected = path == NULL ? NULL : spisor_vector_read(path);
    free(path);
    free(file);
    free(folder);
    return expected;
}

static void print_value(const char *label, const cJSON *value) {
    char *printed = value == NULL ? NULL : cJSON_PrintUnformatted(value);
    fprintf(stderr, "  %s: %s\n", label, value == NULL ? "(absent)" : printed == NULL ? "(unprintable)" : printed);
    cJSON_free(printed);
}

static void compare_case(const char *name, const cJSON *observed, const cJSON *expected) {
    parity_difference difference = {.path = {0}, .observed = NULL, .expected = NULL};
    if (parity_first_difference(observed, expected, &difference)) {
        fprintf(stderr, "%s differs from parity/expected at %s\n", name, difference.path);
        print_value("observed", difference.observed);
        print_value("expected", difference.expected);
        check_failures++;
    }
}

static void check_case(const runtime_services *services, const char *name) {
    staged_case staged = {
        .name = name, .definition = NULL, .root = NULL, .home = NULL, .portable = NULL, .plugin = NULL,
        .names = SPISOR_INIT(spisor_value_tree), .config = SPISOR_INIT(spisor_value_tree), .start = NULL,
        .times = NULL, .no_color = 0, .log_index = 0};
    cJSON *expected = expected_of(name);
    cJSON *observed = expected != NULL && stage_case(name, &staged) ? observe_case(services, &staged) : NULL;
    if (observed == NULL) {
        fprintf(stderr, "%s: could not be observed, or parity/expected has no answer for it\n", name);
        check_failures++;
    } else {
        compare_case(name, observed, expected);
    }
    cJSON_Delete(observed);
    cJSON_Delete(expected);
    release_staged(&staged);
}

static int is_listed(const cJSON *names, const char *name) {
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, names) {
        if (strcmp(entry->valuestring, name) == 0) {
            return 1;
        }
    }
    return 0;
}

/** Fails for an expected answer whose case is gone, which would otherwise hold the runtime to nothing unnoticed. */
static void check_no_stale_expectation(const cJSON *cases) {
    char *folder = parity_join(PARITY_PATH, "expected");
    cJSON *files = folder == NULL ? NULL : parity_names_in(folder, 0);
    const cJSON *file = NULL;
    CHECK(files != NULL);
    cJSON_ArrayForEach(file, files) {
        size_t length = strlen(file->valuestring);
        char *name = malloc(length + 1);
        if (name == NULL) {
            check_failures++;
            break;
        }
        memcpy(name, file->valuestring, length + 1);
        if (length > 5 && strcmp(name + length - 5, ".json") == 0) {
            name[length - 5] = '\0';
        }
        if (!is_listed(cases, name)) {
            fprintf(stderr, "parity/expected/%s answers no case under parity/cases\n", file->valuestring);
            check_failures++;
        }
        free(name);
    }
    cJSON_Delete(files);
    free(folder);
}

static void check_every_case(const runtime_services *services) {
    char *folder = parity_join(PARITY_PATH, "cases");
    cJSON *cases = folder == NULL ? NULL : parity_names_in(folder, 1);
    free(folder);
    if (cases == NULL || cJSON_GetArraySize(cases) == 0) {
        fprintf(stderr, "no cases were found, so the run would hold the runtime to nothing\n");
        check_failures++;
        cJSON_Delete(cases);
        return;
    }
    const cJSON *name = NULL;
    cJSON_ArrayForEach(name, cases) {
        check_case(services, name->valuestring);
    }
    check_no_stale_expectation(cases);
    printf("parity: %d cases observed\n", cJSON_GetArraySize(cases));
    cJSON_Delete(cases);
}

int main(void) {
    spisor_json_use_library_allocator();
    library_handle library = load_runtime();
    if (library == NULL) {
        fprintf(stderr, "could not load %s\n", RUNTIME_LIBRARY_PATH);
        return 1;
    }
    runtime_services services = {
        .paths = definition_of(library, "runtime_paths_definition_of"),
        .config = definition_of(library, "runtime_config_definition_of"),
        .log = definition_of(library, "runtime_log_definition_of")};
    CHECK(services.paths != NULL && services.config != NULL && services.log != NULL);
    if (services.paths != NULL && services.config != NULL && services.log != NULL) {
        check_every_case(&services);
    }
    unload_runtime(library);
    return check_summary("parity_test");
}
