#ifndef HOST_TEST_HOST_FIXTURE_H
#define HOST_TEST_HOST_FIXTURE_H

#include "install_fixture.h"

#include "child_process.h"
#include "file_times.h"
#include "process_alive.h"
#include "registry.h"

#if defined(_WIN32)
#include "command_line_windows.h"
#include "wide_path.h"
#define EXECUTABLE_ENDING ".exe"
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#define EXECUTABLE_ENDING ""
#endif

#define LOADER_ID "fixture-loader"
#define APPLICATION_ID "fixture-app"
#define BACKGROUND_ID "background-fixture"
#define SESSION_PLUGIN "viewer"
#define INTERVAL_MILLISECONDS 300
#define INTERVAL_TEXT "300"
/** What a background host may take beyond its holder checks to see it is unheld and exit. */
#define STOP_ALLOWANCE 4000
#define START_TIMEOUT 30000

/** One run of an executable this test started: its process, where its streams went and what it answered. */
typedef struct program {
    child_process child;
    char out_path[1200];
    char err_path[1200];
    int exit_code;
    char *out;
    char *err;
} program;

static inline char *copy_text(const char *text) {
    char *copy = malloc(strlen(text) + 1);
    strcpy(copy, text);
    return copy;
}

#if defined(_WIN32)

static inline wchar_t *wide_of(const char *text) {
    int units = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    wchar_t *wide = malloc((size_t)units * sizeof *wide);
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, units);
    return wide;
}

static inline wchar_t *program_line(const char *executable, const cJSON *arguments) {
    size_t capacity = strlen(executable) * 2 + 8;
    for (const cJSON *argument = arguments->child; argument != NULL; argument = argument->next) {
        capacity += 2 * strlen(argument->valuestring) + 4;
    }
    wchar_t *line = malloc(capacity * sizeof *line);
    wchar_t *program_name = wide_of(executable);
    size_t used = 0;
    line[used++] = L'"';
    wcscpy(line + used, program_name);
    used += wcslen(program_name);
    line[used++] = L'"';
    for (const cJSON *argument = arguments->child; argument != NULL; argument = argument->next) {
        wchar_t *wide = wide_of(argument->valuestring);
        used = runtime_append_quoted_argument(line, used, wide);
        free(wide);
    }
    line[used] = 0;
    free(program_name);
    return line;
}

static inline wchar_t *program_environment(const cJSON *environment) {
    size_t capacity = 2;
    for (const cJSON *entry = environment->child; entry != NULL; entry = entry->next) {
        capacity += 2 * (strlen(entry->string) + strlen(entry->valuestring)) + 4;
    }
    wchar_t *block = malloc(capacity * sizeof *block);
    size_t used = 0;
    for (const cJSON *entry = environment->child; entry != NULL; entry = entry->next) {
        char *line = malloc(strlen(entry->string) + strlen(entry->valuestring) + 2);
        sprintf(line, "%s=%s", entry->string, entry->valuestring);
        wchar_t *wide = wide_of(line);
        wcscpy(block + used, wide);
        used += wcslen(wide) + 1;
        free(wide);
        free(line);
    }
    block[used++] = 0;
    block[used] = 0;
    return block;
}

static inline HANDLE inherited_output(const char *path) {
    SECURITY_ATTRIBUTES inherited = {sizeof inherited, NULL, TRUE};
    wchar_t *wide = wide_of(path);
    HANDLE file = CreateFileW(wide, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &inherited,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(wide);
    return file;
}

/**
 * Starts executable with arguments under environment, its streams in files; with no terminal when
 * detached, which a hidden console would otherwise give it.
 */
static inline void program_start(program *run, const char *executable, const cJSON *arguments, const cJSON *environment,
                                 const char *directory, int detached) {
    SECURITY_ATTRIBUTES inherited = {sizeof inherited, NULL, TRUE};
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherited, OPEN_EXISTING, 0, NULL);
    HANDLE output = inherited_output(run->out_path);
    HANDLE error = inherited_output(run->err_path);
    STARTUPINFOW startup;
    memset(&startup, 0, sizeof startup);
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = output;
    startup.hStdError = error;
    PROCESS_INFORMATION information;
    wchar_t *line = program_line(executable, arguments);
    wchar_t *block = program_environment(environment);
    wchar_t *folder = wide_of(directory);
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | (detached ? DETACHED_PROCESS : CREATE_NO_WINDOW);
    run->child.pid = 0;
    run->child.handle = NULL;
    if (CreateProcessW(NULL, line, NULL, NULL, TRUE, flags, block, folder, &startup, &information)) {
        CloseHandle(information.hThread);
        run->child.pid = (long)information.dwProcessId;
        run->child.handle = information.hProcess;
    } else {
        fprintf(stderr, "could not start %s: %lu\n", executable, (unsigned long)GetLastError());
    }
    free(folder);
    free(block);
    free(line);
    CloseHandle(input);
    CloseHandle(output);
    CloseHandle(error);
}

static inline int program_wait(program *run, unsigned milliseconds) {
    if (run->child.handle == NULL || WaitForSingleObject(run->child.handle, milliseconds) != WAIT_OBJECT_0) {
        return 0;
    }
    DWORD code = 0;
    GetExitCodeProcess(run->child.handle, &code);
    run->exit_code = (int)code;
    return 1;
}

#else

static inline void program_start(program *run, const char *executable, const cJSON *arguments, const cJSON *environment,
                                 const char *directory, int detached) {
    (void)detached;
    size_t count = (size_t)cJSON_GetArraySize(arguments);
    char **values = calloc(count + 2, sizeof *values);
    values[0] = (char *)executable;
    size_t index = 1;
    for (const cJSON *argument = arguments->child; argument != NULL; argument = argument->next) {
        values[index++] = argument->valuestring;
    }
    size_t variables_count = (size_t)cJSON_GetArraySize(environment);
    char **variables = calloc(variables_count + 1, sizeof *variables);
    index = 0;
    for (const cJSON *entry = environment->child; entry != NULL; entry = entry->next) {
        variables[index] = malloc(strlen(entry->string) + strlen(entry->valuestring) + 2);
        sprintf(variables[index++], "%s=%s", entry->string, entry->valuestring);
    }
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int input = open("/dev/null", O_RDONLY);
        int output = open(run->out_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        int error = open(run->err_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        dup2(input, 0);
        dup2(output, 1);
        dup2(error, 2);
        if (chdir(directory) != 0) {
            _exit(126);
        }
        execve(executable, values, variables);
        _exit(127);
    }
    for (index = 0; index < variables_count; index++) {
        free(variables[index]);
    }
    free(variables);
    free(values);
    run->child.pid = pid > 0 ? (long)pid : 0;
}

static inline int program_wait(program *run, unsigned milliseconds) {
    double started = runtime_now_milliseconds();
    for (;;) {
        int status = 0;
        pid_t waited = run->child.pid <= 0 ? -1 : waitpid((pid_t)run->child.pid, &status, WNOHANG);
        if (waited == (pid_t)run->child.pid) {
            run->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
            run->child.pid = 0;
            return 1;
        }
        if (waited < 0 || runtime_now_milliseconds() - started > milliseconds) {
            return 0;
        }
        pause_milliseconds(10);
    }
}

#endif

static inline char *read_text_file(const char *path) {
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(path, &length);
    char *text = malloc(length + 1);
    if (bytes != NULL) {
        memcpy(text, bytes, length);
    }
    text[bytes == NULL ? 0 : length] = 0;
    free(bytes);
    return text;
}

/** Waits for run to exit, killing it after milliseconds, and reads what it wrote; its exit code, -1 when killed. */
static inline int program_finish(program *run, unsigned milliseconds) {
    if (!program_wait(run, milliseconds)) {
        fprintf(stderr, "a program did not exit within %u ms and was killed\n", milliseconds);
        kill_child(&run->child);
        run->exit_code = -1;
    }
    release_child(&run->child);
    run->out = read_text_file(run->out_path);
    run->err = read_text_file(run->err_path);
    return run->exit_code;
}

static inline void program_free(program *run) {
    free(run->out);
    free(run->err);
    run->out = NULL;
    run->err = NULL;
}

static inline void prepare_program(program *run, const char *streams, const char *label) {
    memset(run, 0, sizeof *run);
    snprintf(run->out_path, sizeof run->out_path, "%s/%s.out", streams, label);
    snprintf(run->err_path, sizeof run->err_path, "%s/%s.err", streams, label);
}

/** A list of the strings given, ended by NULL, as one JSON array. */
static inline cJSON *strings_of(const char *const *values) {
    cJSON *list = cJSON_CreateArray();
    for (; values != NULL && *values != NULL; values++) {
        cJSON_AddItemToArray(list, cJSON_CreateString(*values));
    }
    return list;
}

/** Writes a copy of the file at source to path, keeping it runnable. */
static inline void copy_file(const char *source, const char *path) {
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(source, &length);
    CHECK(bytes != NULL);
    char *parent = joined(path, "..");
    CHECK(spisor_make_directories(parent) == 1);
    CHECK(spisor_replace_file(path, bytes, length) == 1);
#if !defined(_WIN32)
    CHECK(chmod(path, 0755) == 0);
#endif
    free(parent);
    free(bytes);
}

/** A FileRef to catalog/files/<id>/<version>/<name> holding a copy of the file at source. */
static inline cJSON *served_copy(install_base *base, const char *id, const char *version, const char *name, const char *source) {
    char rest[512];
    snprintf(rest, sizeof rest, "files/%s/%s/%s", id, version, name);
    char *path = joined(base->catalog, rest);
    copy_file(source, path);
    char *url = runtime_catalog_file_url_of_path(RUNTIME_HOST_IS_WINDOWS, path);
    char sha256[RUNTIME_SHA256_HEX_SIZE];
    uint64_t size = 0;
    CHECK(runtime_sha256_file_sized(path, sha256, &size));
    cJSON *reference = cJSON_CreateObject();
    cJSON_AddStringToObject(reference, "url", url);
    cJSON_AddStringToObject(reference, "sha256", sha256);
    cJSON_AddNumberToObject(reference, "size", (double)size);
    free(url);
    free(path);
    return reference;
}

/** One test's world: its places, a bin folder for the fake application, the signals it reports through. */
typedef struct host_world {
    install_base base;
    char *bin;
    char *signals;
    char *streams;
    char *starts;
    char *portable_secrets;
    int runs;
} host_world;

static inline void start_world_at(host_world *world, const char *suite, const char *label) {
    start_install_base(&world->base, suite, label);
    world->bin = joined(world->base.root, "bin");
    world->signals = joined(world->base.root, "signals");
    world->streams = joined(world->base.root, "streams");
    world->starts = joined(world->base.root, "starts.txt");
    world->portable_secrets = joined(world->base.root, "secrets.json");
    world->runs = 0;
    CHECK(spisor_make_directories(world->bin) == 1 && spisor_make_directories(world->signals) == 1 &&
          spisor_make_directories(world->streams) == 1);
}

static inline void finish_world(host_world *world) {
    free(world->bin);
    free(world->signals);
    free(world->streams);
    free(world->starts);
    free(world->portable_secrets);
    for (int attempt = 0; attempt < 50 && runtime_remove_tree(world->base.root) != 1; attempt++) {
        pause_milliseconds(200);
    }
    finish_install_base(&world->base);
}

/** The fake application as fixture-app on the world's bin folder. */
static inline void install_fake_application(const host_world *world) {
    char *path = joined(world->bin, APPLICATION_ID EXECUTABLE_ENDING);
    copy_file(FAKE_APPLICATION_PATH, path);
    free(path);
}

static inline void copy_system_variable(cJSON *environment, const char *name) {
    const char *value = getenv(name);
    if (value != NULL) {
        cJSON_AddStringToObject(environment, name, value);
    }
}

/** The environment every program of the world runs under, PATH being path. */
static inline cJSON *world_environment_with_path(const host_world *world, const char *path) {
    cJSON *environment = environment_with_path(&world->base, path);
    cJSON_AddStringToObject(environment, "SPISOR_HOME", world->base.portable);
    cJSON_AddStringToObject(environment, "SPISOR_STATE", world->base.state);
    cJSON_AddStringToObject(environment, "SPISOR_CACHE", world->base.cache);
    cJSON_AddStringToObject(environment, "SPISOR_SECRETS", world->portable_secrets);
    cJSON_AddStringToObject(environment, "LOCALAPPDATA", world->base.root);
    cJSON_AddStringToObject(environment, "FAKE_APPLICATION_SIGNALS", world->signals);
    cJSON_AddStringToObject(environment, "SPISOR_FIXTURE_INTERVAL", INTERVAL_TEXT);
    cJSON_AddStringToObject(environment, "SPISOR_FIXTURE_STARTS", world->starts);
    copy_system_variable(environment, "SystemRoot");
    copy_system_variable(environment, "ComSpec");
    copy_system_variable(environment, "PATHEXT");
    copy_system_variable(environment, "TEMP");
    copy_system_variable(environment, "TMP");
    return environment;
}

static inline cJSON *world_environment(const host_world *world) {
    return world_environment_with_path(world, world->bin);
}

/** Starts executable with the NULL-ended arguments under the world's environment and path. */
static inline void world_start(host_world *world, program *run, const char *executable, const char *const *arguments,
                               const char *path, int detached) {
    char label[32];
    snprintf(label, sizeof label, "run-%d", ++world->runs);
    prepare_program(run, world->streams, label);
    cJSON *list = strings_of(arguments);
    cJSON *environment = world_environment_with_path(world, path == NULL ? world->bin : path);
    program_start(run, executable, list, environment, world->base.root, detached);
    cJSON_Delete(environment);
    cJSON_Delete(list);
}

/** Runs forebay-host's test build with the NULL-ended arguments to its end; its exit code. */
static inline int world_run(host_world *world, program *run, const char *const *arguments, const char *path) {
    world_start(world, run, FOREBAY_HOST_PATH, arguments, path, 1);
    return program_finish(run, 120000);
}

static inline void expect_contains(const char *text, const char *part, const char *label) {
    if (text == NULL || strstr(text, part) == NULL) {
        fprintf(stderr, "%s: expected to find \"%s\" in \"%s\"\n", label, part, text == NULL ? "(none)" : text);
        check_failures++;
    }
}

/** Lists file key in the platform files of version of item, a copy of source. */
static inline void list_platform_copy(install_base *base, cJSON *item, const char *id, const char *version, const char *key,
                                      const char *source) {
    cJSON *versions = cJSON_GetObjectItemCaseSensitive(item, "versions");
    cJSON *entry = cJSON_GetObjectItemCaseSensitive(versions, version);
    entry = entry != NULL ? entry : cJSON_AddObjectToObject(versions, version);
    cJSON *platforms = cJSON_GetObjectItemCaseSensitive(entry, "platforms");
    platforms = platforms != NULL ? platforms : cJSON_AddObjectToObject(entry, "platforms");
    cJSON *platform = cJSON_GetObjectItemCaseSensitive(platforms, runtime_platform_key());
    platform = platform != NULL ? platform : cJSON_AddObjectToObject(platforms, runtime_platform_key());
    cJSON *files = cJSON_GetObjectItemCaseSensitive(platform, "files");
    files = files != NULL ? files : cJSON_AddObjectToObject(platform, "files");
    cJSON_AddItemToObject(files, key, served_copy(base, id, version, key, source));
}

/** forebay-host at version as the one command would publish it: its executable, forebay-boot and the launcher shims copy. */
static inline void list_native_forebay_host(install_base *base, const char *version) {
    cJSON *item = catalog_item(base, "forebay-host", "host", "native", version);
    char key[256];
    snprintf(key, sizeof key, "%s/forebay-host" EXECUTABLE_ENDING, runtime_platform_key());
    list_platform_copy(base, item, "forebay-host", version, key, FOREBAY_HOST_PATH);
    snprintf(key, sizeof key, "%s/forebay-boot" EXECUTABLE_ENDING, runtime_platform_key());
    list_platform_copy(base, item, "forebay-host", version, key, FOREBAY_BOOT_PATH);
#if defined(_WIN32)
    snprintf(key, sizeof key, "%s/runtime-launcher.exe", runtime_platform_key());
    list_platform_copy(base, item, "forebay-host", version, key, RUNTIME_LAUNCHER_PATH);
#endif
}

/** A native background host whose layer is the fixture, its host.json naming it as the entry. */
static inline void list_native_background(install_base *base, const char *id, int with_entry) {
    cJSON *item = catalog_item(base, id, "host", "native", "1.0.0");
    cJSON *entry = cJSON_AddObjectToObject(cJSON_GetObjectItemCaseSensitive(item, "versions"), "1.0.0");
    cJSON *files = cJSON_AddObjectToObject(entry, "files");
    const char *manifest = with_entry ? "{\"entry\": \"background-host" EXECUTABLE_ENDING "\"}" : "{}";
    cJSON_AddItemToObject(files, "host.json", served_reference(base, id, "1.0.0", "host.json", manifest));
    cJSON_AddItemToObject(files, "background-host" EXECUTABLE_ENDING,
                          served_copy(base, id, "1.0.0", "background-host" EXECUTABLE_ENDING, LAYER_FIXTURE_PATH));
}

/** The launch ecosystem: forebay-host, the runtime, the fixture loader naming commands and the background host. */
static inline void list_launch_catalog(host_world *world, const char *commands_json, int javascript_plugin) {
    install_base *base = &world->base;
    list_native_forebay_host(base, "1.0.0");
    list_library(base, "runtime", "0.1.0");
    list_loader(base, LOADER_ID, "1.0.0", application_json(APPLICATION_ID, APPLICATION_ID, "{\"fake\": \"fixture-pkg\"}"),
                commands_json, 0, "[\"" BACKGROUND_ID "\"]");
    list_native_background(base, BACKGROUND_ID, 1);
    if (javascript_plugin) {
        list_plugin(base, SESSION_PLUGIN, "1.0.0", "javascript");
        list_library(base, "koffi", "2.16.3");
        cJSON *item = catalog_item(base, "javascript-host", "host", "javascript", "1.0.0");
        cJSON *entry = cJSON_AddObjectToObject(cJSON_GetObjectItemCaseSensitive(item, "versions"), "1.0.0");
        cJSON *files = cJSON_AddObjectToObject(entry, "files");
        cJSON_AddItemToObject(files, "host.json", served_reference(base, "javascript-host", "1.0.0", "host.json", "{\"entry\": \"host.js\"}"));
        cJSON_AddItemToObject(files, "host.js", served_reference(base, "javascript-host", "1.0.0", "host.js", "export {};\n"));
    }
    write_catalog(base);
}

/** A node on its own folder, the layer fixture, which answers a version install reuses and runs a layer when asked. */
static inline char *install_fake_node(const host_world *world) {
    char *folder = joined(world->base.root, "node-bin");
    char *path = joined(folder, "node" EXECUTABLE_ENDING);
    copy_file(LAYER_FIXTURE_PATH, path);
    free(path);
    return folder;
}

/** forebay-host install --catalog <catalog> with extra arguments, the NULL-ended rest; its exit code. */
static inline int world_install(host_world *world, const char *const *extra, const char *path) {
    char *index = index_path_of(&world->base);
    const char *arguments[16] = {"install", "--catalog", index};
    size_t count = 3;
    for (; extra != NULL && *extra != NULL && count < 15; extra++) {
        arguments[count++] = *extra;
    }
    arguments[count] = NULL;
    program run;
    int code = world_run(world, &run, arguments, path);
    if (code != 0) {
        fprintf(stderr, "install answered %d: %s\n%s\n", code, run.out, run.err);
    }
    program_free(&run);
    free(index);
    return code;
}

static inline cJSON *registration_in(const host_world *world, const char *id) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "places", places_of(&world->base));
    cJSON_AddStringToObject(argument, "id", id);
    catalog_answer answer = call_catalog(runtime_registry_get, argument);
    cJSON *registration = cJSON_DetachItemFromObjectCaseSensitive(answer.tree, "registration");
    cJSON_Delete(answer.tree);
    return registration;
}

/** What the fake application tagged tag recorded when it started, NULL before it has. */
static inline cJSON *application_start(const host_world *world, const char *tag) {
    char name[256];
    snprintf(name, sizeof name, "%s.started", tag);
    return json_in(world->signals, name);
}

static inline cJSON *await_application(const host_world *world, const char *tag) {
    double started = runtime_now_milliseconds();
    cJSON *start = NULL;
    while ((start = application_start(world, tag)) == NULL && runtime_now_milliseconds() - started < START_TIMEOUT) {
        pause_milliseconds(25);
    }
    if (start == NULL) {
        fprintf(stderr, "the application %s did not start\n", tag);
        check_failures++;
    }
    return start;
}

static inline void release_application(const host_world *world, const char *tag) {
    char name[256];
    snprintf(name, sizeof name, "%s.release", tag);
    write_in(world->signals, name, "");
}

/** The holders file of host id, or NULL when there is none. */
static inline cJSON *holders_of(const host_world *world, const char *id) {
    char name[256];
    snprintf(name, sizeof name, "processes/%s.json", id);
    return json_in(world->base.state, name);
}

static inline int holder_count(const cJSON *holders) {
    return cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(holders, "holders"));
}

/** The pid the holders file of id records once it names count holders; 0 when it never does. */
static inline long await_holders(const host_world *world, const char *id, int count) {
    double started = runtime_now_milliseconds();
    while (runtime_now_milliseconds() - started < START_TIMEOUT) {
        cJSON *holders = holders_of(world, id);
        const cJSON *pid = cJSON_GetObjectItemCaseSensitive(holders, "pid");
        long found = cJSON_IsNumber(pid) && holder_count(holders) == count ? (long)pid->valuedouble : 0;
        cJSON_Delete(holders);
        if (found != 0) {
            return found;
        }
        pause_milliseconds(25);
    }
    fprintf(stderr, "%s was never held by %d\n", id, count);
    check_failures++;
    return 0;
}

static inline int gone_within(long pid, double milliseconds) {
    double started = runtime_now_milliseconds();
    while (runtime_process_is_alive((double)pid)) {
        if (runtime_now_milliseconds() - started > milliseconds) {
            return 0;
        }
        pause_milliseconds(25);
    }
    return 1;
}

/** Hard-kills pid when it still runs, a test's last word on anything it started. */
static inline void stop_if_alive(long pid) {
    if (pid > 0 && runtime_process_is_alive((double)pid)) {
        kill_pid(pid);
    }
}

#endif
