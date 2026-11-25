#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "host_fixture.h"

#include "atomic.h"
#include "catalog_hooks.h"
#include "fetch_hooks.h"
#include "file_times.h"
#include "http_fixture.h"
#include "spisor/json_allocation.h"
#include "thread.h"

/**
 * @file
 * A plugin's `config.get` through its scope on one thread while another runs a blocking `tick` of `weivel.host`, as a
 * host program calls `tick` on a worker (rev 225). The tick's update check asks an https catalog source the loopback
 * fixture answers only after STALL_MILLISECONDS, so the tick waits on I/O as a real one does.
 */

#define STALL_MILLISECONDS 1500
#define SOURCE "https://catalog.test"
#define VIEWER_MANIFEST                                                                                             \
    "{\"id\": \"a\", \"version\": \"1.0.0\", \"entry\": \"index.js\", "                                            \
    "\"config\": {\"name\": \"a\", \"defaults\": {\"theme\": \"dark\"}}}"

typedef struct blocking_tick {
    weivel_status status;
    volatile long finished;
} blocking_tick;

static void tick_once(void *argument) {
    blocking_tick *blocked = argument;
    const weivel_standard_host *host = host_definition();
    weivel_standard_host_next next = WEIVEL_INIT(weivel_standard_host_next);
    blocked->status = host->tick(host->context, 1700000000000LL, &next, NULL);
    host->release_next(host->context, &next);
    runtime_atomic_increment(&blocked->finished);
}

static int has_finished(blocking_tick *blocked) {
#if defined(_WIN32)
    return InterlockedCompareExchange(&blocked->finished, 0, 0) != 0;
#else
    return __atomic_load_n(&blocked->finished, __ATOMIC_ACQUIRE) != 0;
#endif
}

static int reads_theme(const weivel_standard_config *config) {
    weivel_standard_setting_answer answer = WEIVEL_INIT(weivel_standard_setting_answer);
    int read = config->get(config->context, "theme", &answer, NULL) == WEIVEL_STATUS_OK &&
               answer.value.kind == WEIVEL_STANDARD_SETTING_TEXT && strcmp(answer.value.text, "dark") == 0;
    config->release_answer(config->context, &answer);
    return read;
}

/** The loader's settings reading the https source alone, which the fixture serves after its stall. */
static void read_the_stalled_source(void) {
    char *text;
    cJSON *settings = parsed("{\"plugins\": {\"a\": {\"enabled\": true}}, \"catalog\": {\"sources\": "
                             "[{\"kind\": \"https\", \"url\": \"" SOURCE "/index.json\"}]}}");
    text = cJSON_PrintUnformatted(settings);
    write_in(application_home, "config/settings.json", text);
    cJSON_free(text);
    cJSON_Delete(settings);
}

static char *index_text(void) {
    char *path = index_path();
    size_t length = 0;
    char *text = (char *)spisor_read_file_reporting(path, &length, &(spisor_read_outcome){SPISOR_READ_FAILED});
    spisor_memory_release(path);
    return text;
}

static void started_with_its_plugin(const started_run *run) {
    const weivel_standard_host *host = host_definition();
    weivel_standard_host_outcome outcome = {run->plan.plugins[0].manifest, WEIVEL_STANDARD_HOST_ACTIVATED, NULL};
    CHECK(host->started(host->context, &outcome, 1, NULL) == WEIVEL_STATUS_OK);
}

/**
 * Reads the config until the tick returns, requiring every read to finish while the tick still waits: the slowest read
 * far below the stall, so no read waited for the tick, and reads made in the middle of the stall.
 */
static void reads_while(const weivel_standard_config *config, blocking_tick *blocked) {
    long reads = 0;
    long failed_reads = 0;
    long reads_mid_stall = 0;
    double slowest = 0;
    double began = runtime_now_milliseconds();
    double took;
    runtime_thread ticker;
    CHECK(runtime_thread_start(&ticker, tick_once, blocked));
    while (!has_finished(blocked)) {
        double read_began = runtime_now_milliseconds();
        int read = reads_theme(config);
        double read_took = runtime_now_milliseconds() - read_began;
        slowest = read_took > slowest ? read_took : slowest;
        reads += read;
        failed_reads += !read;
        reads_mid_stall += read && read_began - began >= STALL_MILLISECONDS / 3 &&
                           read_began - began <= 2 * STALL_MILLISECONDS / 3;
    }
    runtime_thread_join(&ticker);
    took = runtime_now_milliseconds() - began;
    printf("%ld reads of config.get while a tick blocked for %.0f ms, %ld of them mid-stall, the slowest %.0f ms, "
           "%ld failed\n",
           reads, took, reads_mid_stall, slowest, failed_reads);
    CHECK(failed_reads == 0 && blocked->status == WEIVEL_STATUS_OK && took >= STALL_MILLISECONDS / 2);
    CHECK(reads_mid_stall > 0 && slowest < STALL_MILLISECONDS / 4);
}

static void config_reads_while_a_tick_blocks(void) {
    const char *arguments[2] = {LAYER_RUNTIME, LOADER};
    const weivel_standard_scopes *scopes = runtime_standard_scope_definition_of();
    weivel_standard_scope scope = WEIVEL_INIT(weivel_standard_scope);
    blocking_tick blocked = {WEIVEL_STATUS_MALFORMED, 0};
    http_route route = {"/index.json", 200, NULL, NULL, 0, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, STALL_MILLISECONDS, 0};
    char target[64];
    char *body;
    char *seen;
    started_run run;
    http_fixture *fixture;
    start_held_host_world("tick-threads");
    install_plugin("a", VIEWER_MANIFEST);
    read_the_stalled_source();
    body = index_text();
    route.body = body;
    route.body_length = body == NULL ? 0 : strlen(body);
    fixture = http_fixture_start(&route, 1);
    snprintf(target, sizeof target, "http://127.0.0.1:%u", (unsigned)http_fixture_port(fixture));
    runtime_catalog_redirect_requests(SOURCE, target);
    runtime_fetch_allow_plain_http(1);
    run = start_with(arguments, 2);
    CHECK(run.status == WEIVEL_STATUS_OK && run.plan.plugin_count == 1);
    if (run.status == WEIVEL_STATUS_OK && run.plan.plugin_count == 1) {
        started_with_its_plugin(&run);
        CHECK(scopes->open(scopes->context, run.plan.plugins[0].manifest, &scope, NULL) == WEIVEL_STATUS_OK);
    }
    if (scope.config != NULL) {
        reads_while(scope.config, &blocked);
        scopes->release_scope(scopes->context, &scope);
    }
    release_run(&run);
    if (run.status == WEIVEL_STATUS_OK) {
        stop_expecting(0, "a serve run whose tick blocked");
    }
    seen = http_fixture_stop(fixture);
    CHECK(seen != NULL && strstr(seen, "GET /index.json") != NULL);
    spisor_memory_release(seen);
    runtime_catalog_redirect_requests(NULL, NULL);
    runtime_fetch_allow_plain_http(0);
    spisor_memory_release(body);
    finish_base();
}

int main(void) {
    spisor_json_use_library_allocator();
    config_reads_while_a_tick_blocks();
    return check_summary("host_tick_threads_test");
}
