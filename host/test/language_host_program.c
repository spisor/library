/**
 * @file
 * A host program for plugins written in C, built on weivel/c's `host` module, standing in for the program weivel/c does
 * not ship: run as `<program> <runtime binary> <language> <loader id> [...]`, it binds `weivel.host` and `weivel.scope`
 * from the runtime binary, runs what `start` plans, each plugin's entry read from its shared library under
 * C_FIXTURE_ENTRY_SYMBOL, and realises an action's method through c_fixture_method_service. It runs only the language
 * HOST_LANGUAGE names, refusing a start for any other, so a layer started from another language's item fails, and
 * appends "<pid> <language>-host <arguments...>" to the file SPISOR_FIXTURE_STARTS names, as the layer fixture does.
 *
 * @implNote It loads no library a plan names and offers a plugin none of its scope's services, which no C fixture
 * plugin needs; a plan naming a library ends the run.
 */

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <weivel/c/host.h>
#include <weivel/host.h>
#include <weivel/scope.h>
#include <weivel/standard.h>

#include "c_fixture_service.h"

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#define process_id() _getpid()
#define CORE_LIBRARY "weivel_core.dll"
#define SEPARATORS "\\/"
#else
#include <dlfcn.h>
#include <unistd.h>
#define process_id() getpid()
#define SEPARATORS "/"
#if defined(__APPLE__)
#define CORE_LIBRARY "libweivel_core.dylib"
#else
#define CORE_LIBRARY "libweivel_core.so"
#endif
#endif

#define HOST_ID HOST_LANGUAGE "-host"
/** No layer outlives a broken test by more than this. */
#define LONGEST_LIFE_MILLISECONDS 120000
#define WAIT_STEP_MILLISECONDS 50
#define DETAIL_SIZE 512

static volatile sig_atomic_t stop_requested = 0;

static void request_stop(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static int64_t now_milliseconds(void) {
    struct timespec now;
    timespec_get(&now, TIME_UTC);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void pause_milliseconds(long milliseconds) {
#if defined(_WIN32)
    Sleep((DWORD)milliseconds);
#else
    struct timespec pause = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
    nanosleep(&pause, NULL);
#endif
}

static void record_start(int count, char **arguments) {
    const char *starts = getenv("SPISOR_FIXTURE_STARTS");
    FILE *file = starts == NULL ? NULL : fopen(starts, "ab");
    if (file == NULL) {
        return;
    }
    fprintf(file, "%ld " HOST_ID, (long)process_id());
    for (int index = 1; index < count; index++) {
        fprintf(file, " %s", arguments[index]);
    }
    fputs("\n", file);
    fclose(file);
}

/** The runtime binary the program was started with, and the two definitions it serves. */
typedef struct runtime_binary {
    void *library;
    const weivel_standard_host *host;
    const weivel_standard_scopes *scopes;
} runtime_binary;

typedef const void *(*definition_getter)(void);

static definition_getter getter_of(void *library, const char *name) {
    definition_getter getter = NULL;
#if defined(_WIN32)
    FARPROC symbol = GetProcAddress((HMODULE)library, name);
    memcpy(&getter, &symbol, sizeof getter);
#else
    void *symbol = dlsym(library, name);
    memcpy(&getter, &symbol, sizeof getter);
#endif
    return getter;
}

static int open_runtime(const char *path, runtime_binary *runtime) {
#if defined(_WIN32)
    int units = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    wchar_t *wide = units <= 0 ? NULL : malloc((size_t)units * sizeof *wide);
    if (wide != NULL) {
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, units);
        runtime->library = LoadLibraryW(wide);
    }
    free(wide);
#else
    runtime->library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
    definition_getter host = runtime->library == NULL ? NULL : getter_of(runtime->library, "runtime_standard_host_definition_of");
    definition_getter scopes = runtime->library == NULL ? NULL
                                                        : getter_of(runtime->library, "runtime_standard_scope_definition_of");
    runtime->host = host == NULL ? NULL : host();
    runtime->scopes = scopes == NULL ? NULL : scopes();
    return runtime->host != NULL;
}

static void close_runtime(runtime_binary *runtime) {
#if defined(_WIN32)
    FreeLibrary((HMODULE)runtime->library);
#else
    dlclose(runtime->library);
#endif
}

/** The core's binary, which stage-bundle puts beside the program's own. */
static char *core_path_beside(const char *program) {
#if defined(_WIN32)
    wchar_t wide[4096];
    DWORD units = GetModuleFileNameW(NULL, wide, 4096);
    char own[8192];
    int bytes = units == 0 ? 0 : WideCharToMultiByte(CP_UTF8, 0, wide, (int)units, own, (int)sizeof own - 1, NULL, NULL);
    own[bytes > 0 ? bytes : 0] = 0;
#else
    char own[8192];
    snprintf(own, sizeof own, "%s", program);
#endif
    (void)program;
    size_t directory = 0;
    for (size_t index = 0; own[index] != 0; index++) {
        if (strchr(SEPARATORS, own[index]) != NULL) {
            directory = index + 1;
        }
    }
    char *path = malloc(directory + sizeof CORE_LIBRARY);
    if (path != NULL) {
        memcpy(path, own, directory);
        memcpy(path + directory, CORE_LIBRARY, sizeof CORE_LIBRARY);
    }
    return path;
}

static char *read_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    char *text = NULL;
    if (file != NULL && fseek(file, 0, SEEK_END) == 0) {
        long size = ftell(file);
        text = size < 0 || fseek(file, 0, SEEK_SET) != 0 ? NULL : malloc((size_t)size + 1);
        if (text != NULL && fread(text, 1, (size_t)size, file) == (size_t)size) {
            text[size] = 0;
            *length = (size_t)size;
        } else {
            free(text);
            text = NULL;
        }
    }
    if (file != NULL) {
        fclose(file);
    }
    return text;
}

/** One plugin of the plan as the program ran it: what it told `started`, and what it holds until the run ends. */
typedef struct planned_plugin {
    weivel_standard_manifest *manifest;
    weivel_standard_scope scope;
    int scope_open;
    weivel_handle handle;
    uint32_t state;
    char detail[DETAIL_SIZE];
} planned_plugin;

/** What the listener heard of the one plugin an activation started. */
typedef struct heard_outcome {
    uint32_t kind;
    char detail[DETAIL_SIZE];
} heard_outcome;

static weivel_c_status hear(void *data, const weivel_c_outcome *outcome) {
    heard_outcome *heard = data;
    heard->kind = outcome->kind;
    snprintf(heard->detail, sizeof heard->detail, "%s",
             outcome->refusal.detail == NULL ? "the activation did not finish" : outcome->refusal.detail);
    return WEIVEL_C_STATUS_OK;
}

static void refuse_plugin(planned_plugin *plugin, uint32_t state, const char *first, const char *second) {
    plugin->state = state;
    snprintf(plugin->detail, sizeof plugin->detail, "%s%s", first, second == NULL ? "" : second);
}

static int read_manifest(planned_plugin *plugin, const char *path) {
    size_t length = 0;
    char *json = read_file(path, &length);
    weivel_standard_refusal refusal = WEIVEL_INIT(weivel_standard_refusal);
    int read = json != NULL && weivel_standard_read_manifest(json, length, &plugin->manifest, &refusal) == WEIVEL_STATUS_OK;
    if (!read) {
        refuse_plugin(plugin, WEIVEL_STANDARD_HOST_REFUSED, "the manifest cannot be read: ",
                      refusal.detail == NULL ? path : refusal.detail);
    }
    weivel_standard_refusal_release(&refusal);
    free(json);
    return read;
}

static int open_scope(planned_plugin *plugin, const runtime_binary *runtime, const char *manifest_path) {
    weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
    plugin->scope = (weivel_standard_scope)WEIVEL_INIT(weivel_standard_scope);
    if (runtime->scopes == NULL) {
        return 1;
    }
    plugin->scope_open = runtime->scopes->open(runtime->scopes->context, manifest_path, &plugin->scope, &refusal) ==
                         WEIVEL_STATUS_OK;
    if (!plugin->scope_open) {
        refuse_plugin(plugin, WEIVEL_STANDARD_HOST_REFUSED, "the scope was refused: ",
                      refusal.detail == NULL ? "no reason given" : refusal.detail);
        runtime->scopes->release_refusal(runtime->scopes->context, &refusal);
    }
    return plugin->scope_open;
}

static int registered(planned_plugin *plugin, weivel_c_host *host, const char *entry_path) {
    weivel_c_entry entry = WEIVEL_INIT(weivel_c_entry);
    if (weivel_c_entry_load(entry_path, C_FIXTURE_ENTRY_SYMBOL, &entry, NULL) != WEIVEL_C_STATUS_OK) {
        refuse_plugin(plugin, WEIVEL_STANDARD_HOST_FAILED, "the entry cannot be loaded: ", entry_path);
        return 0;
    }
    weivel_c_refusal refusal = WEIVEL_INIT(weivel_c_refusal);
    if (weivel_c_host_register(host, weivel_standard_manifest_view(plugin->manifest), &entry, &plugin->handle,
                               &refusal) != WEIVEL_C_STATUS_OK) {
        weivel_c_entry_unload(&entry);
        refuse_plugin(plugin, WEIVEL_STANDARD_HOST_REFUSED, "the core refused the plugin: ",
                      refusal.detail == NULL ? "no reason given" : refusal.detail);
        return 0;
    }
    return 1;
}

static void activate(planned_plugin *plugin, weivel_c_host *host) {
    heard_outcome heard = {0, {0}};
    weivel_c_activation activation = WEIVEL_INIT(weivel_c_activation);
    weivel_activation_plan plan = WEIVEL_INIT(weivel_activation_plan);
    activation.plugins = &plugin->handle;
    activation.count = 1;
    activation.edge_source = WEIVEL_EDGES_SERVICES;
    activation.listener.hear = hear;
    activation.listener.data = &heard;
    if (weivel_c_host_activate(host, &activation, &plan, NULL) == WEIVEL_C_STATUS_OK) {
        weivel_c_host_plan_release(host, &plan);
    }
    if (heard.kind == WEIVEL_C_OUTCOME_ACTIVE) {
        plugin->state = WEIVEL_STANDARD_HOST_ACTIVATED;
    } else {
        refuse_plugin(plugin, heard.kind == WEIVEL_C_OUTCOME_REFUSED ? WEIVEL_STANDARD_HOST_REFUSED
                                                                       : WEIVEL_STANDARD_HOST_FAILED,
                      heard.kind == 0 ? "the activation was heard of no outcome" : heard.detail, NULL);
    }
}

/** Opens the plugin's scope and activates it, as the plan's order asks, each refusal kept as its outcome. */
static void start_plugin(planned_plugin *plugin, const weivel_standard_host_plugin *planned, weivel_c_host *host,
                         const runtime_binary *runtime) {
    if (read_manifest(plugin, planned->manifest) && open_scope(plugin, runtime, planned->manifest) &&
        registered(plugin, host, planned->entry)) {
        activate(plugin, host);
    }
}

static void serve(const runtime_binary *runtime) {
    const weivel_standard_host *host = runtime->host;
    int64_t began = now_milliseconds();
    while (!stop_requested && now_milliseconds() - began < LONGEST_LIFE_MILLISECONDS) {
        weivel_standard_host_next next = WEIVEL_INIT(weivel_standard_host_next);
        weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
        if (host->tick(host->context, now_milliseconds(), &next, &refusal) != WEIVEL_STATUS_OK) {
            host->release_refusal(host->context, &refusal);
            return;
        }
        uint32_t has_next = next.has_next;
        int64_t due = next.next;
        host->release_next(host->context, &next);
        if (has_next > 1) {
            return;
        }
        while (!stop_requested && now_milliseconds() - began < LONGEST_LIFE_MILLISECONDS &&
               (has_next == 0 || now_milliseconds() < due)) {
            pause_milliseconds(WAIT_STEP_MILLISECONDS);
        }
    }
}

static int provides(const weivel_manifest *manifest, const char *service) {
    for (uint32_t index = 0; index < manifest->services.provides.count; index++) {
        if (strcmp(manifest->services.provides.items[index].id, service) == 0) {
            return 1;
        }
    }
    return 0;
}

/** Runs the plan's action, writing its result's text, or why it failed, into answer; 1 when it answered. */
static int run_method(const weivel_standard_host_plan *plan, const planned_plugin *plugin, weivel_c_host *host,
                      char *answer, size_t capacity) {
    weivel_c_context *context = NULL;
    void *service = NULL;
    if (plan->action_service == NULL || plan->action_method == NULL) {
        snprintf(answer, capacity, "a C plugin's entry offers no action, so %s needs a call", plan->action_name);
        return 0;
    }
    if (!provides(weivel_standard_manifest_view(plugin->manifest), plan->action_service)) {
        snprintf(answer, capacity, "the plugin does not provide %s", plan->action_service);
        return 0;
    }
    if (weivel_c_host_context(host, &context) != WEIVEL_C_STATUS_OK ||
        weivel_c_context_get_service(context, plan->action_service, &service, NULL) != WEIVEL_C_STATUS_OK) {
        snprintf(answer, capacity, "the plugin registered no %s", plan->action_service);
        return 0;
    }
    const c_fixture_method_service *method = service;
    return method->call(method->data, plan->action_method, &plan->action_arguments, answer, capacity);
}

static void act(const runtime_binary *runtime, const weivel_standard_host_plan *plan, const planned_plugin *plugins,
                weivel_c_host *host) {
    const planned_plugin *plugin = NULL;
    for (size_t index = 0; index < plan->plugin_count; index++) {
        if (strcmp(plan->plugins[index].manifest, plan->action_plugin) == 0) {
            plugin = &plugins[index];
        }
    }
    char answer[DETAIL_SIZE];
    int answered = 0;
    if (plugin == NULL || plugin->state != WEIVEL_STANDARD_HOST_ACTIVATED) {
        snprintf(answer, sizeof answer, "the action's plugin was not activated");
    } else {
        answered = run_method(plan, plugin, host, answer, sizeof answer);
    }
    weivel_standard_value result = {0};
    result.kind = WEIVEL_STANDARD_VALUE_TEXT;
    result.text = answer;
    result.length = strlen(answer);
    weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
    if (runtime->host->finished(runtime->host->context, answered ? &result : NULL, answered ? NULL : answer, &refusal) !=
        WEIVEL_STATUS_OK) {
        runtime->host->release_refusal(runtime->host->context, &refusal);
    }
}

static int tell_started(const runtime_binary *runtime, const weivel_standard_host_plan *plan, planned_plugin *plugins) {
    weivel_standard_host_outcome *outcomes = calloc(plan->plugin_count + 1, sizeof *outcomes);
    weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
    if (outcomes == NULL) {
        return 0;
    }
    for (size_t index = 0; index < plan->plugin_count; index++) {
        outcomes[index].manifest = plan->plugins[index].manifest;
        outcomes[index].state = plugins[index].state;
        outcomes[index].detail = plugins[index].state == WEIVEL_STANDARD_HOST_ACTIVATED ? NULL : plugins[index].detail;
    }
    int told = runtime->host->started(runtime->host->context, outcomes, plan->plugin_count, &refusal) == WEIVEL_STATUS_OK;
    if (!told) {
        fprintf(stderr, "started was refused: %s\n", refusal.detail == NULL ? "no reason given" : refusal.detail);
        runtime->host->release_refusal(runtime->host->context, &refusal);
    }
    free(outcomes);
    return told;
}

static void end_plugins(const runtime_binary *runtime, weivel_c_host *host, planned_plugin *plugins, size_t count) {
    uint32_t finished = 0;
    if (host != NULL) {
        weivel_c_host_destroy(host, &finished);
    }
    for (size_t index = 0; index < count; index++) {
        if (plugins[index].scope_open) {
            runtime->scopes->release_scope(runtime->scopes->context, &plugins[index].scope);
        }
        weivel_standard_manifest_release(plugins[index].manifest);
    }
}

/** Runs what the plan says up to the point `stop` is due; the host the plugins ran in, freed after `stop`. */
static weivel_c_host *run_plan(const runtime_binary *runtime, const weivel_standard_host_plan *plan, const char *program) {
    if (plan->mode != WEIVEL_STANDARD_HOST_SERVE && plan->mode != WEIVEL_STANDARD_HOST_ACTION) {
        return NULL;
    }
    if (plan->library_count > 0) {
        fprintf(stderr, HOST_ID " loads no library, and the plan names %s\n", plan->libraries[0].binary);
        return NULL;
    }
    char *core = core_path_beside(program);
    weivel_c_host_options options = WEIVEL_INIT(weivel_c_host_options);
    options.core_path = core;
    weivel_c_host *host = NULL;
    weivel_c_status created = core == NULL ? WEIVEL_C_STATUS_OUT_OF_MEMORY : weivel_c_host_create(&options, &host, NULL);
    free(core);
    planned_plugin *plugins = calloc(plan->plugin_count + 1, sizeof *plugins);
    if (created != WEIVEL_C_STATUS_OK || plugins == NULL) {
        fprintf(stderr, HOST_ID " could not make its host: %u\n", (unsigned)created);
        free(plugins);
        if (host != NULL) {
            weivel_c_host_free(host);
        }
        return NULL;
    }
    for (size_t index = 0; index < plan->plugin_count && !stop_requested; index++) {
        start_plugin(&plugins[index], &plan->plugins[index], host, runtime);
    }
    if (tell_started(runtime, plan, plugins) && !stop_requested) {
        if (plan->mode == WEIVEL_STANDARD_HOST_SERVE) {
            serve(runtime);
        } else {
            act(runtime, plan, plugins, host);
        }
    }
    end_plugins(runtime, host, plugins, plan->plugin_count);
    free(plugins);
    return host;
}

static int run(int count, char **arguments) {
    if (count < 4) {
        fputs("usage: " HOST_ID " <runtime binary> <language> <loader id> [...]\n", stderr);
        return 2;
    }
    if (strcmp(arguments[2], HOST_LANGUAGE) != 0) {
        fprintf(stderr, HOST_ID " runs plugins written in " HOST_LANGUAGE ", not %s\n", arguments[2]);
        return 2;
    }
    signal(SIGINT, request_stop);
    signal(SIGTERM, request_stop);
    record_start(count, arguments);
    runtime_binary runtime = {NULL, NULL, NULL};
    if (!open_runtime(arguments[1], &runtime)) {
        fprintf(stderr, HOST_ID " could not bind weivel.host from %s\n", arguments[1]);
        return 1;
    }
    const weivel_standard_host *host = runtime.host;
    weivel_standard_host_plan plan = WEIVEL_INIT(weivel_standard_host_plan);
    weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
    if (host->start(host->context, (const char *const *)arguments + 2, (size_t)count - 2, &plan, &refusal) !=
        WEIVEL_STATUS_OK) {
        fprintf(stderr, "%s\nfix: %s\n", refusal.detail == NULL ? "start was refused" : refusal.detail,
                refusal.fix == NULL ? "none given" : refusal.fix);
        host->release_refusal(host->context, &refusal);
        close_runtime(&runtime);
        return 1;
    }
    weivel_c_host *ran = run_plan(&runtime, &plan, arguments[0]);
    host->release_plan(host->context, &plan);
    uint32_t exit_code = 1;
    int stopped = host->stop(host->context, &exit_code, &refusal) == WEIVEL_STATUS_OK;
    if (!stopped) {
        host->release_refusal(host->context, &refusal);
    }
    if (ran != NULL) {
        weivel_c_host_free(ran);
    }
    close_runtime(&runtime);
    return stopped ? (int)exit_code : 1;
}

#if defined(_WIN32)

int wmain(int count, wchar_t **wide) {
    char **arguments = calloc((size_t)count + 1, sizeof *arguments);
    for (int index = 0; arguments != NULL && index < count; index++) {
        int bytes = WideCharToMultiByte(CP_UTF8, 0, wide[index], -1, NULL, 0, NULL, NULL);
        arguments[index] = malloc((size_t)(bytes > 0 ? bytes : 1));
        if (arguments[index] != NULL) {
            WideCharToMultiByte(CP_UTF8, 0, wide[index], -1, arguments[index], bytes, NULL, NULL);
        }
    }
    signal(SIGBREAK, request_stop);
    int code = arguments == NULL ? 2 : run(count, arguments);
    for (int index = 0; arguments != NULL && index < count; index++) {
        free(arguments[index]);
    }
    free(arguments);
    return code;
}

#else

int main(int count, char **arguments) {
    return run(count, arguments);
}

#endif
