#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "host_fixture.h"

#include "child_process.h"

#include "spisor/json_allocation.h"

/**
 * @file
 * Calls `weivel.host`'s started, tick, finished and stop as the host program does, after a start over a registered
 * loader, and requires the ready record a SERVE run publishes, the timers a tick acts on, the run its holders end, the
 * exit code each way a run ends answers, and the refusal of every call out of its order.
 */

#define NOW 1700000000000LL
#define TURN 5000LL
#define CHECK_INTERVAL 3600000LL
#define FAILURES_BEFORE_EXIT 6

static const char *const serve_arguments[2] = {LAYER_RUNTIME, LOADER};
static const char *const in_process_arguments[3] = {LAYER_RUNTIME, LOADER, "in-process"};
static const char *const action_arguments[6] = {LAYER_RUNTIME, LOADER, "action", "a", "greet", "{}"};

typedef struct tick_answer {
    weivel_status status;
    weivel_standard_host_next next;
    weivel_standard_service_refusal refusal;
} tick_answer;

/** Starts the run the arguments name, requiring an OK start with one plugin. */
static started_run start_one(const char *const *arguments, size_t count) {
    started_run run = start_with(arguments, count);
    CHECK(run.status == WEIVEL_STATUS_OK && run.plan.plugin_count == 1);
    return run;
}

static weivel_standard_host_outcome outcome_of(const started_run *run, uint32_t plugin_state, const char *detail) {
    const char *manifest = run->plan.plugin_count == 1 ? run->plan.plugins[0].manifest : "";
    weivel_standard_host_outcome outcome = {manifest, plugin_state, detail};
    return outcome;
}

static weivel_status tell_started(const weivel_standard_host_outcome *outcomes, size_t count,
                                  weivel_standard_service_refusal *refusal) {
    const weivel_standard_host *host = host_definition();
    if (refusal != NULL) {
        memset(refusal, 0, sizeof *refusal);
        refusal->size = sizeof *refusal;
    }
    return host->started(host->context, outcomes, count, refusal);
}

static void release_refusal(weivel_standard_service_refusal *refusal) {
    const weivel_standard_host *host = host_definition();
    host->release_refusal(host->context, refusal);
}

static tick_answer tick_at(int64_t now) {
    const weivel_standard_host *host = host_definition();
    tick_answer answer;
    memset(&answer, 0, sizeof answer);
    answer.next.size = sizeof answer.next;
    answer.refusal.size = sizeof answer.refusal;
    answer.status = host->tick(host->context, now, &answer.next, &answer.refusal);
    return answer;
}

static void release_tick(tick_answer *answer) {
    const weivel_standard_host *host = host_definition();
    host->release_next(host->context, &answer->next);
    host->release_refusal(host->context, &answer->refusal);
}

static void expect_detail(const weivel_standard_service_refusal *refusal, const char *part, const char *label) {
    if (refusal->detail == NULL || strstr(refusal->detail, part) == NULL) {
        fprintf(stderr, "%s: expected a refusal naming \"%s\", got \"%s\"\n", label, part,
                refusal->detail == NULL ? "(none)" : refusal->detail);
        check_failures++;
    }
}

/** Ticks at `now`, requiring OK with the next tick at `next`. */
static void expect_tick(int64_t now, int64_t next, const char *label) {
    tick_answer answer = tick_at(now);
    if (answer.status != WEIVEL_STATUS_OK || answer.next.has_next != 1 || answer.next.next != next) {
        fprintf(stderr, "%s: expected OK with the next tick at %lld, got %d with %u at %lld (%s)\n", label,
                (long long)next, (int)answer.status, (unsigned)answer.next.has_next, (long long)answer.next.next,
                answer.refusal.detail == NULL ? "no refusal" : answer.refusal.detail);
        check_failures++;
    }
    release_tick(&answer);
}

static void expect_tick_refused(int64_t now, const char *part, const char *label) {
    tick_answer answer = tick_at(now);
    if (answer.status != WEIVEL_STATUS_REFUSED || answer.next.has_next != 0) {
        fprintf(stderr, "%s: expected a refused tick, got %d\n", label, (int)answer.status);
        check_failures++;
    }
    expect_detail(&answer.refusal, part, label);
    release_tick(&answer);
}

/** The ready record the last run of identity published, parsed, or NULL when there is none. */
static cJSON *ready_record_of(const char *identity) {
    char *problem = NULL;
    char *path = runtime_holders_path(state, identity, ".ready.json", &problem);
    size_t length = 0;
    char *text = (char *)spisor_read_file_reporting(path, &length, &(spisor_read_outcome){SPISOR_READ_FAILED});
    cJSON *record = text == NULL ? NULL : spisor_parse_json_reporting((const uint8_t *)text, length, &(int){0});
    spisor_memory_release(text);
    spisor_memory_release(path);
    spisor_memory_release(problem);
    return record;
}

/** The ready record the loader's last run published, parsed, or NULL when there is none. */
static cJSON *ready_record(void) {
    return ready_record_of(LOADER);
}

/** When the loader's holders record says its host was last alive, which a prune stamps with the tick's now. */
static double holders_alive_at(void) {
    char *path = processes_file(".json");
    runtime_holders_record record = runtime_no_holders();
    double alive_at;
    CHECK(runtime_read_holders(path, &record) == 1);
    alive_at = record.alive_at;
    runtime_free_holders(&record);
    spisor_memory_release(path);
    return alive_at;
}

/** When the loader's one run record says its run was last alive, which a tick's turn stamps with its now. */
static double run_record_alive_at(void) {
    runtime_directory_listing listing = {NULL, 0};
    char *directory = joined(state, "bundles/runs/" LOADER);
    char *path = NULL;
    char *text = NULL;
    cJSON *record = NULL;
    double alive_at = -1;
    size_t length = 0;
    CHECK(runtime_list_directory(directory, &listing) == SPISOR_OK && listing.count == 1);
    path = listing.count == 1 ? joined(directory, listing.entries[0].name) : NULL;
    text = path == NULL ? NULL
                        : (char *)spisor_read_file_reporting(path, &length,
                                                             &(spisor_read_outcome){SPISOR_READ_FAILED});
    record = text == NULL ? NULL : spisor_parse_json_reporting((const uint8_t *)text, length, &(int){0});
    if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(record, "aliveAt"))) {
        alive_at = cJSON_GetObjectItemCaseSensitive(record, "aliveAt")->valuedouble;
    }
    cJSON_Delete(record);
    spisor_memory_release(text);
    spisor_memory_release(path);
    spisor_memory_release(directory);
    runtime_free_directory_listing(&listing);
    return alive_at;
}

/** A SERVE run told its one plugin was activated, with this process holding the loader. */
static started_run serving(const char *label) {
    weivel_standard_host_outcome outcome;
    started_run run;
    start_held_host_world(label);
    run = start_one(serve_arguments, 2);
    outcome = outcome_of(&run, WEIVEL_STANDARD_HOST_ACTIVATED, NULL);
    CHECK(tell_started(&outcome, 1, NULL) == WEIVEL_STATUS_OK);
    return run;
}

static void a_serve_run_started_publishes_that_it_is_ready(void) {
    started_run run = serving("ready");
    cJSON *record = ready_record();
    expect_json(cJSON_GetObjectItemCaseSensitive(record, "environment"), "{}", "the ready record's environment");
    CHECK(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(record, "pid")) == (double)current_process_id());
    CHECK(cJSON_GetObjectItemCaseSensitive(record, "refused") == NULL);
    cJSON_Delete(record);
    release_run(&run);
    stop_expecting(0, "a ready serve run");
    finish_base();
}

#define PROXY_STANDARD                                                                                           \
    "{\"plugins\": [\"a\"], \"application\": {\"proxy\": {\"baseUrlVariable\": \"ANTHROPIC_BASE_URL\", "        \
    "\"placeholder\": {\"variable\": \"ANTHROPIC_AUTH_TOKEN\", \"value\": \"sk-ant-loader-proxy\"}}}}"
#define LISTENING_MANIFEST                                                                                        \
    "{\"id\": \"a\", \"version\": \"1.0.0\", \"entry\": \"index.js\", "                                         \
    "\"config\": {\"name\": \"a\", \"defaults\": {\"proxy\": {\"port\": 34567}}}, "                             \
    "\"access\": [{\"kind\": \"network\", \"target\": \"example.com\", \"mode\": \"connect\"}, "                \
    "{\"kind\": \"port\", \"target\": \"setting:proxy.port\", \"mode\": \"listen\"}]}"

/** The loader's standard at the version its registration names, as install leaves it in the cache. */
static void write_loader_standard(const char *standard) {
    write_in(cache, "bundles/" LOADER "/1.0.0/host.json", standard);
}

/**
 * A SERVE run of a loader with `standard`, `a` installed with `manifest` and its portable settings `settings`, told
 * `a` activated.
 */
static started_run serving_with_standard(const char *label, const char *manifest, const char *settings,
                                         const char *standard) {
    weivel_standard_host_outcome outcome;
    started_run run;
    start_held_host_world(label);
    install_plugin("a", manifest);
    write_loader_standard(standard);
    write_in(portable, "plugins/a/config.json", settings);
    run = start_one(serve_arguments, 2);
    outcome = outcome_of(&run, WEIVEL_STANDARD_HOST_ACTIVATED, NULL);
    CHECK(tell_started(&outcome, 1, NULL) == WEIVEL_STATUS_OK);
    return run;
}

/** A SERVE run of a loader whose standard requires `a` as its front door, as serving_with_standard runs it. */
static started_run serving_front_door_set(const char *label, const char *manifest, const char *settings) {
    return serving_with_standard(label, manifest, settings, PROXY_STANDARD);
}

/** serving_front_door_set with the user's port 43123 over the manifest's default. */
static started_run serving_front_door(const char *label, const char *manifest) {
    return serving_front_door_set(label, manifest, "{\"proxy\": {\"port\": 43123}}");
}

static void expect_front_door_environment(const char *manifest, const char *label) {
    started_run run = serving_front_door(label, manifest);
    cJSON *record = ready_record();
    expect_json(cJSON_GetObjectItemCaseSensitive(record, "environment"),
                "{\"ANTHROPIC_BASE_URL\": \"http://127.0.0.1:43123\", "
                "\"ANTHROPIC_AUTH_TOKEN\": \"sk-ant-loader-proxy\"}",
                label);
    CHECK(cJSON_GetObjectItemCaseSensitive(record, "refused") == NULL);
    cJSON_Delete(record);
    release_run(&run);
    stop_expecting(0, label);
    finish_base();
}

/**
 * The front door listens on the port its manifest declares by the setting `proxy.port`, which the user set to 43123
 * over its default, so the application is handed that address and the placeholder its standard declares; a port
 * declared as a number is handed as it stands.
 */
static void a_front_door_serve_run_publishes_its_applications_variables(void) {
    expect_front_door_environment(LISTENING_MANIFEST, "a front door listening on its setting");
    expect_front_door_environment(
        "{\"id\": \"a\", \"version\": \"1.0.0\", \"entry\": \"index.js\", "
        "\"access\": [{\"kind\": \"port\", \"target\": 43123, \"mode\": \"listen\"}]}",
        "a front door listening on a declared number");
}

static void expect_front_door_refused(started_run run, const char *refused_part, const char *fix, const char *label) {
    cJSON *record = ready_record();
    const char *refused = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(record, "refused"));
    CHECK(cJSON_GetObjectItemCaseSensitive(record, "environment") == NULL);
    if (refused == NULL || strstr(refused, refused_part) == NULL) {
        fprintf(stderr, "%s: expected a refusal holding \"%s\", got \"%s\"\n", label, refused_part,
                refused == NULL ? "(null)" : refused);
        check_failures++;
    }
    check_text(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(record, "fix")), fix, label);
    cJSON_Delete(record);
    release_run(&run);
    stop_expecting(0, label);
    finish_base();
}

/**
 * A front door that declares no fixed port it listens on, or whose setting holds no port, cannot be handed to the
 * application, so the launch is refused rather than told an empty environment.
 */
static void a_front_door_with_no_port_to_hand_publishes_a_refusal(void) {
    expect_front_door_refused(serving_front_door("front-door-without-port", PLUGIN_MANIFEST),
                              "a, the front door of " LOADER ", declares no fixed port it listens on",
                              "install a version of a that declares the port it listens on as port access with "
                              "mode listen",
                              "a front door declaring no port");
    expect_front_door_refused(
        serving_front_door("front-door-on-any-port",
                           "{\"id\": \"a\", \"version\": \"1.0.0\", \"entry\": \"index.js\", "
                           "\"access\": [{\"kind\": \"port\", \"target\": \"any\", \"mode\": \"listen\"}]}"),
        "a, the front door of " LOADER ", declares no fixed port it listens on",
        "install a version of a that declares the port it listens on as port access with mode listen",
        "a front door listening on a port the system picks");
    expect_front_door_refused(serving_front_door_set("front-door-port-setting-no-port", LISTENING_MANIFEST,
                                                     "{\"proxy\": {\"port\": \"high\"}}"),
                              "a, the front door of " LOADER ", listens on the port its setting proxy.port holds, "
                              "which is no port from 1 to 65535",
                              "set proxy.port of a to a port from 1 to 65535",
                              "a front door whose setting holds no port");
}

#define PORT_SETTING "{\"proxy\": {\"port\": 43123}}"

/** A standard whose proxy lacks a member the environment needs names it, rather than answering out of memory. */
static void a_proxy_the_standard_cannot_describe_publishes_a_refusal_naming_the_member(void) {
    const char *standards[3] = {
        "{\"plugins\": [\"a\"], \"application\": {\"proxy\": {\"placeholder\": {\"variable\": \"TOKEN\", "
        "\"value\": \"placeholder\"}}}}",
        "{\"plugins\": [\"a\"], \"application\": {\"proxy\": {\"baseUrlVariable\": \"WHERE\", "
        "\"placeholder\": {\"value\": \"placeholder\"}}}}",
        "{\"plugins\": [\"a\"], \"application\": {\"proxy\": {\"baseUrlVariable\": \"WHERE\", "
        "\"placeholder\": {\"variable\": \"TOKEN\", \"value\": 7}}}}"};
    const char *members[3] = {"application.proxy.baseUrlVariable", "application.proxy.placeholder.variable",
                              "application.proxy.placeholder.value"};
    for (size_t index = 0; index < 3; index++) {
        char expected[256];
        snprintf(expected, sizeof expected, "the standard of " LOADER " declares a proxy whose %s is not text",
                 members[index]);
        expect_front_door_refused(serving_with_standard(members[index], LISTENING_MANIFEST, PORT_SETTING,
                                                        standards[index]),
                                  expected, "run spisor-host install --catalog <path or url> again", members[index]);
    }
}

/** A front door the run does not hold, or a standard requiring no plugin, leaves no address to hand. */
static void a_proxy_with_no_front_door_to_read_publishes_a_refusal(void) {
    expect_front_door_refused(
        serving_with_standard("front-door-not-installed", LISTENING_MANIFEST, PORT_SETTING,
                              "{\"plugins\": [\"z\"], \"application\": {\"proxy\": {\"baseUrlVariable\": \"WHERE\", "
                              "\"placeholder\": {\"variable\": \"TOKEN\", \"value\": \"placeholder\"}}}}"),
        "z, the front door of " LOADER ", is not installed, so its application cannot be handed the address",
        "run spisor-host install --catalog <path or url> again", "a front door that is not installed");
    expect_front_door_refused(
        serving_with_standard("standard-requiring-no-plugin", LISTENING_MANIFEST, PORT_SETTING,
                              "{\"application\": {\"proxy\": {\"baseUrlVariable\": \"WHERE\", "
                              "\"placeholder\": {\"variable\": \"TOKEN\", \"value\": \"placeholder\"}}}}"),
        LOADER "'s standard declares a proxy but requires no plugin to serve it",
        "run spisor-host install --catalog <path or url> again", "a standard requiring no plugin");
}

/** A setting the front door declares but neither its defaults nor any layer set is named as not set. */
static void a_front_door_port_setting_that_is_not_set_publishes_a_refusal(void) {
    expect_front_door_refused(
        serving_front_door_set("front-door-port-not-set",
                               "{\"id\": \"a\", \"version\": \"1.0.0\", \"entry\": \"index.js\", "
                               "\"access\": [{\"kind\": \"port\", \"target\": \"setting:proxy.port\", "
                               "\"mode\": \"listen\"}]}",
                               "{}"),
        "a, the front door of " LOADER ", listens on the port its setting proxy.port, which is not set",
        "set proxy.port of a to a port from 1 to 65535", "a front door whose setting is not set");
}

/** A manifest that turned unreadable after the plugin activated is named as unreadable, not as declaring no port. */
static void a_front_door_whose_manifest_cannot_be_read_publishes_a_refusal(void) {
    weivel_standard_host_outcome outcome;
    started_run run;
    start_held_host_world("front-door-unreadable-manifest");
    install_plugin("a", LISTENING_MANIFEST);
    write_loader_standard(PROXY_STANDARD);
    run = start_one(serve_arguments, 2);
    outcome = outcome_of(&run, WEIVEL_STANDARD_HOST_ACTIVATED, NULL);
    write_file(outcome.manifest, "{");
    CHECK(tell_started(&outcome, 1, NULL) == WEIVEL_STATUS_OK);
    expect_front_door_refused(run, "a, the front door of " LOADER ", has a manifest at ",
                              "run spisor-host install --catalog <path or url> again",
                              "a front door whose manifest cannot be read");
}

/** The port is the one the plugin reads: the home's config layer over the portable one, over the default. */
static void a_front_door_port_set_in_the_home_wins_over_the_portable_one(void) {
    weivel_standard_host_outcome outcome;
    started_run run;
    cJSON *record;
    start_held_host_world("front-door-home-port");
    install_plugin("a", LISTENING_MANIFEST);
    write_loader_standard(PROXY_STANDARD);
    write_in(portable, "plugins/a/config.json", "{\"proxy\": {\"port\": 34000}}");
    write_in(application_home, "config/a.json", PORT_SETTING);
    run = start_one(serve_arguments, 2);
    outcome = outcome_of(&run, WEIVEL_STANDARD_HOST_ACTIVATED, NULL);
    CHECK(tell_started(&outcome, 1, NULL) == WEIVEL_STATUS_OK);
    record = ready_record();
    expect_json(cJSON_GetObjectItemCaseSensitive(record, "environment"),
                "{\"ANTHROPIC_BASE_URL\": \"http://127.0.0.1:43123\", "
                "\"ANTHROPIC_AUTH_TOKEN\": \"sk-ant-loader-proxy\"}",
                "a port the home sets over the portable one");
    cJSON_Delete(record);
    release_run(&run);
    stop_expecting(0, "a port the home sets");
    finish_base();
}

/**
 * The javascript layer of a loader whose front door is a native plugin runs as `<loader>.javascript`, and hands the
 * application nothing: only the front door's layer publishes the proxy's variables.
 */
static void a_layer_not_holding_the_front_door_publishes_an_empty_environment(void) {
    weivel_standard_host_outcome outcome;
    started_run run;
    cJSON *record;
    start_held_host_world("other-layer-environment");
    install_plugin("a", LISTENING_MANIFEST);
    list_plugin("n", "1.0.0", "1.0.0");
    cJSON_AddStringToObject(cJSON_GetObjectItemCaseSensitive(catalog_items, "n"), "runtime", "native");
    write_catalog();
    write_loader_standard("{\"plugins\": [\"n\"], \"application\": {\"proxy\": {\"baseUrlVariable\": \"WHERE\", "
                          "\"placeholder\": {\"variable\": \"TOKEN\", \"value\": \"placeholder\"}}}}");
    run = start_one(serve_arguments, 2);
    outcome = outcome_of(&run, WEIVEL_STANDARD_HOST_ACTIVATED, NULL);
    CHECK(tell_started(&outcome, 1, NULL) == WEIVEL_STATUS_OK);
    record = ready_record_of(LOADER ".javascript");
    expect_json(cJSON_GetObjectItemCaseSensitive(record, "environment"), "{}", "the other layer's environment");
    cJSON_Delete(record);
    release_run(&run);
    stop_expecting(0, "a layer not holding the front door");
    finish_base();
}

#define SECOND_MANIFEST "{\"id\": \"b\", \"version\": \"1.0.0\", \"entry\": \"index.js\"}"
/** A loader whose settings enable plugins `a` and `b`, both installed. */
static void start_two_plugin_world(const char *label) {
    cJSON *settings;
    char *text;
    start_host_world(label);
    install_plugin("a", PLUGIN_MANIFEST);
    install_plugin("b", SECOND_MANIFEST);
    settings = settings_with("{\"plugins\": {\"a\": {\"enabled\": true}, \"b\": {\"enabled\": true}}}");
    text = cJSON_PrintUnformatted(settings);
    write_in(application_home, "config/settings.json", text);
    cJSON_free(text);
    cJSON_Delete(settings);
}

#define CONSUMING_MANIFEST                                                                                       \
    "{\"id\": \"a\", \"version\": \"1.0.0\", \"entry\": \"index.js\", "                                          \
    "\"capabilities\": {\"provides\": [], \"consumes\": [\"greeting\"]}, "                                      \
    "\"actions\": {\"greet\": {\"call\": {\"service\": \"a.greeter\", \"method\": \"greet\"}}}}"
#define PROVIDING_MANIFEST(id)                                                                                   \
    "{\"id\": \"" id "\", \"version\": \"1.0.0\", \"entry\": \"index.js\", "                                    \
    "\"capabilities\": {\"provides\": [\"greeting\"], \"consumes\": []}}"

/**
 * A loader whose settings enable `a`, whose action consumes `greeting`, and `b` and `c`, each providing it, so an
 * action of `a` plans all three.
 */
static void start_two_provider_world(const char *label) {
    cJSON *settings;
    char *text;
    start_host_world(label);
    install_plugin("a", CONSUMING_MANIFEST);
    install_plugin("b", PROVIDING_MANIFEST("b"));
    install_plugin("c", PROVIDING_MANIFEST("c"));
    settings = settings_with(
        "{\"plugins\": {\"a\": {\"enabled\": true}, \"b\": {\"enabled\": true}, \"c\": {\"enabled\": true}}}");
    text = cJSON_PrintUnformatted(settings);
    write_in(application_home, "config/settings.json", text);
    cJSON_free(text);
    cJSON_Delete(settings);
}

/** Starts the two-plugin run the arguments name and tells it `a` and `b` ended as `a_state` and `b_state`. */
static started_run started_with_states(const char *const *arguments, size_t count, uint32_t a_state,
                                       uint32_t b_state, const char *detail) {
    weivel_standard_host_outcome outcomes[2];
    started_run run = start_with(arguments, count);
    CHECK(run.status == WEIVEL_STATUS_OK && run.plan.plugin_count == 2);
    if (run.plan.plugin_count == 2) {
        weivel_standard_host_outcome a = {run.plan.plugins[0].manifest, a_state, detail};
        weivel_standard_host_outcome b = {run.plan.plugins[1].manifest, b_state, detail};
        outcomes[0] = a;
        outcomes[1] = b;
        CHECK(tell_started(outcomes, 2, NULL) == WEIVEL_STATUS_OK);
    }
    return run;
}

/** The two-plugin run told `a` and `b` ended as `a_state` and `b_state`, publishing the refusal `expected`. */
static void a_plugin_not_activated_publishes_a_refusal_naming_it(uint32_t a_state, uint32_t b_state,
                                                                 const char *detail, const char *expected,
                                                                 const char *fix, const char *label) {
    started_run run;
    cJSON *record;
    start_two_plugin_world(label);
    hold_the_loader();
    run = started_with_states(serve_arguments, 2, a_state, b_state, detail);
    record = ready_record();
    check_text(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(record, "refused")), expected, label);
    check_text(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(record, "fix")), fix, label);
    CHECK(cJSON_GetObjectItemCaseSensitive(record, "environment") == NULL);
    cJSON_Delete(record);
    expect_tick(NOW, NOW + TURN, label);
    release_run(&run);
    stop_expecting(0, label);
    finish_base();
}

static void outcomes_that_are_not_the_plans_are_malformed(void) {
    weivel_standard_host_outcome outcomes[2];
    started_run run;
    const char *labels[5] = {"no outcome", "two outcomes", "another manifest", "no state", "an unknown state"};
    for (size_t index = 0; index < 5; index++) {
        start_host_world("malformed-outcomes");
        run = start_one(serve_arguments, 2);
        outcomes[0] = outcome_of(&run, WEIVEL_STANDARD_HOST_ACTIVATED, NULL);
        outcomes[1] = outcomes[0];
        if (index == 2) {
            outcomes[0].manifest = "elsewhere/plugin.json";
        }
        outcomes[0].state = index == 3 ? 0u : index == 4 ? 4u : outcomes[0].state;
        if (tell_started(outcomes, index == 0 ? 0 : index == 1 ? 2 : 1, NULL) != WEIVEL_STATUS_MALFORMED) {
            fprintf(stderr, "%s: expected MALFORMED\n", labels[index]);
            check_failures++;
        }
        release_run(&run);
        stop_expecting(RUNTIME_HOST_EXIT_FAILED, labels[index]);
        finish_base();
    }
}

static void a_second_started_is_refused_and_fails_the_run(void) {
    weivel_standard_service_refusal refusal;
    weivel_standard_host_outcome outcome;
    started_run run = serving("second-started");
    outcome = outcome_of(&run, WEIVEL_STANDARD_HOST_ACTIVATED, NULL);
    CHECK(tell_started(&outcome, 1, &refusal) == WEIVEL_STATUS_REFUSED);
    expect_detail(&refusal, "a second time", "a second started");
    release_refusal(&refusal);
    release_run(&run);
    stop_expecting(RUNTIME_HOST_EXIT_FAILED, "a run told started twice");
    finish_base();
}

static weivel_status tell_finished(const weivel_standard_value *result, const char *failure,
                                   weivel_standard_service_refusal *refusal) {
    const weivel_standard_host *host = host_definition();
    if (refusal != NULL) {
        memset(refusal, 0, sizeof *refusal);
        refusal->size = sizeof *refusal;
    }
    return host->finished(host->context, result, failure, refusal);
}

/** An ACTION run told its one plugin was activated, which publishes no ready record. */
static started_run acting(const char *label) {
    weivel_standard_host_outcome outcome;
    started_run run;
    cJSON *record;
    start_host_world(label);
    run = start_one(action_arguments, 6);
    outcome = outcome_of(&run, WEIVEL_STANDARD_HOST_ACTIVATED, NULL);
    CHECK(tell_started(&outcome, 1, NULL) == WEIVEL_STATUS_OK);
    record = ready_record();
    CHECK(record == NULL);
    cJSON_Delete(record);
    return run;
}

static void an_action_answers_its_exit_code_as_it_ended(void) {
    weivel_standard_value answered = {WEIVEL_STANDARD_VALUE_NONE, 0, 0.0, NULL, 0, NULL, NULL, 0};
    weivel_standard_service_refusal refusal;
    started_run run = acting("action-answered");
    expect_tick_refused(NOW, "an ACTION run, which never ticks", "a tick in an action run");
    release_run(&run);
    stop_expecting(RUNTIME_HOST_EXIT_FAILED, "an action run that ticked");
    finish_base();

    run = acting("action-result");
    CHECK(tell_finished(&answered, NULL, NULL) == WEIVEL_STATUS_OK);
    release_run(&run);
    stop_expecting(0, "an action that answered");
    finish_base();

    run = acting("action-threw");
    CHECK(tell_finished(NULL, "it threw", NULL) == WEIVEL_STATUS_OK);
    release_run(&run);
    stop_expecting(RUNTIME_HOST_EXIT_FAILED, "an action that threw");
    finish_base();

    run = acting("action-twice");
    CHECK(tell_finished(&answered, NULL, NULL) == WEIVEL_STATUS_OK);
    CHECK(tell_finished(&answered, NULL, &refusal) == WEIVEL_STATUS_REFUSED);
    expect_detail(&refusal, "or a second time", "a second finished");
    release_refusal(&refusal);
    release_run(&run);
    stop_expecting(0, "an action that answered, told finished twice");
    finish_base();
}

static void finished_is_refused_in_a_serve_run_and_before_started(void) {
    weivel_standard_value answered = {WEIVEL_STANDARD_VALUE_NONE, 0, 0.0, NULL, 0, NULL, NULL, 0};
    weivel_standard_service_refusal refusal;
    started_run run = serving("finished-serve");
    CHECK(tell_finished(&answered, NULL, &refusal) == WEIVEL_STATUS_REFUSED);
    expect_detail(&refusal, "in a SERVE run", "finished in a serve run");
    release_refusal(&refusal);
    release_run(&run);
    stop_expecting(RUNTIME_HOST_EXIT_FAILED, "a serve run told finished");
    finish_base();

    start_host_world("finished-early");
    run = start_one(action_arguments, 6);
    CHECK(tell_finished(&answered, NULL, &refusal) == WEIVEL_STATUS_REFUSED);
    expect_detail(&refusal, "before started answered OK", "finished before started");
    release_refusal(&refusal);
    release_run(&run);
    stop_expecting(RUNTIME_HOST_EXIT_FAILED, "an action told finished before started");
    finish_base();
}

static int is_installed(const char *version) {
    char rest[128];
    snprintf(rest, sizeof rest, "bundles/a/%s/.installed.json", version);
    return exists_in(cache, rest);
}

/** Lists `version` of plugin `a` as the catalog's latest, which only an update check installs. */
static void publish_plugin(const char *version) {
    list_plugin("a", version, version);
    write_catalog();
}

/** An action of `a` beside `b`, which provides what it consumes and activated, and `c`, which also does and failed. */
static void an_action_runs_on_when_another_plugin_was_not_activated(void) {
    weivel_standard_value answered = {WEIVEL_STANDARD_VALUE_NONE, 0, 0.0, NULL, 0, NULL, NULL, 0};
    weivel_standard_host_outcome outcomes[3];
    started_run run;
    start_two_provider_world("action-beside-a-failed-plugin");
    run = start_with(action_arguments, 6);
    CHECK(run.status == WEIVEL_STATUS_OK && run.plan.plugin_count == 3);
    if (run.plan.plugin_count == 3) {
        weivel_standard_host_outcome a = {run.plan.plugins[0].manifest, WEIVEL_STANDARD_HOST_ACTIVATED, NULL};
        weivel_standard_host_outcome b = {run.plan.plugins[1].manifest, WEIVEL_STANDARD_HOST_ACTIVATED, NULL};
        weivel_standard_host_outcome c = {run.plan.plugins[2].manifest, WEIVEL_STANDARD_HOST_FAILED, "it threw"};
        outcomes[0] = a;
        outcomes[1] = b;
        outcomes[2] = c;
        CHECK(tell_started(outcomes, 3, NULL) == WEIVEL_STATUS_OK);
    }
    CHECK(tell_finished(&answered, NULL, NULL) == WEIVEL_STATUS_OK);
    release_run(&run);
    stop_expecting(0, "an action whose plugin activated beside one that failed");
    finish_base();
}

static void a_tick_acts_on_each_timer_when_it_is_due(void) {
    started_run run = serving("timers");
    publish_plugin("1.1.0");
    expect_tick(NOW, NOW + TURN, "the first tick");
    CHECK(holders_alive_at() == (double)NOW);
    CHECK(is_installed("1.1.0"));
    publish_plugin("1.2.0");
    expect_tick(NOW + 1000, NOW + TURN, "a tick before anything is due");
    CHECK(holders_alive_at() == (double)NOW);
    expect_tick(NOW + TURN, NOW + 2 * TURN, "the second turn");
    CHECK(holders_alive_at() == (double)(NOW + TURN));
    CHECK(run_record_alive_at() == (double)(NOW + TURN));
    CHECK(!is_installed("1.2.0"));
    expect_tick(NOW + CHECK_INTERVAL, NOW + CHECK_INTERVAL + TURN, "the second check");
    CHECK(holders_alive_at() == (double)(NOW + CHECK_INTERVAL));
    CHECK(is_installed("1.2.0"));
    release_run(&run);
    stop_expecting(0, "a serve run that ticked");
    finish_base();
}

static void a_late_tick_runs_each_timer_once(void) {
    started_run run = serving("late");
    expect_tick(NOW, NOW + TURN, "the first tick");
    expect_tick(NOW + 7 * TURN + 1, NOW + 8 * TURN + 1, "a tick seven turns late");
    CHECK(run_record_alive_at() == (double)(NOW + 7 * TURN + 1));
    release_run(&run);
    stop_expecting(0, "a serve run ticked late");
    finish_base();
}

/** A tick years behind the clock, as NOW is, keeps its own run record through its update check's cache sweep. */
static void a_tick_far_behind_the_clock_keeps_its_run_alive_through_its_check(void) {
    started_run run = serving("behind");
    expect_tick(NOW, NOW + TURN, "a tick that turns and checks years behind the clock");
    CHECK(run_record_alive_at() == (double)NOW);
    CHECK(holders_alive_at() == (double)NOW);
    release_run(&run);
    stop_expecting(0, "a serve run ticked behind the clock");
    finish_base();
}

static void a_tick_at_the_epoch_reads_no_clock(void) {
    started_run run = serving("epoch");
    expect_tick(-TURN, 0, "a tick before the epoch");
    CHECK(holders_alive_at() == (double)-TURN);
    expect_tick(0, TURN, "a tick at the epoch");
    CHECK(holders_alive_at() == 0);
    CHECK(run_record_alive_at() == 0);
    release_run(&run);
    stop_expecting(0, "a serve run ticked at the epoch");
    finish_base();
}

static void a_serve_run_no_holder_keeps_ends_with_exit_code_zero(void) {
    weivel_standard_host_outcome outcome;
    started_run run;
    start_host_world("unheld");
    run = start_one(serve_arguments, 2);
    outcome = outcome_of(&run, WEIVEL_STANDARD_HOST_ACTIVATED, NULL);
    CHECK(tell_started(&outcome, 1, NULL) == WEIVEL_STATUS_OK);
    expect_tick_refused(NOW, "no holder of " LOADER " is alive", "a tick no holder keeps");
    expect_tick_refused(NOW + TURN, "after the run ended", "a tick after the run ended");
    CHECK(tell_started(&outcome, 1, NULL) == WEIVEL_STATUS_REFUSED);
    release_run(&run);
    stop_expecting(0, "a serve run its holders left");
    finish_base();
}

/** Puts a file where the processes folder is, so every prune is refused, or puts the folder and the holder back. */
static void break_the_prune(int broken) {
    char *folder = joined(state, "processes");
    CHECK(runtime_remove_tree(folder) == SPISOR_OK);
    if (broken) {
        write_file(folder, "not a folder");
    } else {
        hold_the_loader();
    }
    spisor_memory_release(folder);
}

/** The loader's one log file of the day NOW falls on, or NULL when there is none. */
static char *loader_log_of_now(void) {
    runtime_directory_listing listing = {NULL, 0};
    char *directory = joined(application_home, "logs/2023-11-14");
    char *path = NULL;
    char *text = NULL;
    size_t length = 0;
    CHECK(runtime_list_directory(directory, &listing) == SPISOR_OK && listing.count == 1);
    path = listing.count == 1 ? joined(directory, listing.entries[0].name) : NULL;
    if (path != NULL) {
        text = (char *)spisor_read_file_reporting(path, &length, &(spisor_read_outcome){SPISOR_READ_FAILED});
    }
    spisor_memory_release(path);
    spisor_memory_release(directory);
    runtime_free_directory_listing(&listing);
    return text;
}

static void failed_prunes_end_the_run_only_once_enough_fail_in_a_row(void) {
    started_run run = serving("prune-failures");
    int64_t now = NOW;
    char *log;
    break_the_prune(1);
    for (int turn = 1; turn < FAILURES_BEFORE_EXIT; turn++, now += TURN) {
        expect_tick(now, now + TURN, "a failed turn");
    }
    log = loader_log_of_now();
    CHECK(log != NULL && strstr(log, "[2023-11-14T22:13:20.000Z] [WARN] spisor could not check the holders of " LOADER
                                     ": the processes folder") != NULL);
    spisor_memory_release(log);
    break_the_prune(0);
    expect_tick(now, now + TURN, "a turn that succeeds again");
    now += TURN;
    break_the_prune(1);
    for (int turn = 1; turn < FAILURES_BEFORE_EXIT; turn++, now += TURN) {
        expect_tick(now, now + TURN, "a failed turn after a success");
    }
    expect_tick_refused(now, "could not be checked, so its run ends", "the last failed turn");
    release_run(&run);
    stop_expecting(0, "a serve run whose holders could not be checked");
    finish_base();
}

static void a_now_no_check_can_carry_fails_the_run(void) {
    started_run run = serving("far-now");
    expect_tick_refused(253402300800000LL, "outside the years 0 to 9999", "a now in the year 10000");
    release_run(&run);
    stop_expecting(RUNTIME_HOST_EXIT_FAILED, "a serve run given a now no check carries");
    finish_base();
}

static child_process start_sleeper(void) {
    const char *arguments[1] = {"sleep"};
    return start_child(RUNTIME_BACKGROUND_FIXTURE_PATH, arguments, 1, 0, NULL);
}

/** Records `pid` as the loader's host, stamped alive at `alive_at`, held by `holders`, a JSON array. */
static void record_the_host(long pid, int64_t alive_at, const char *holders) {
    char record[200];
    char *path = processes_file(".json");
    snprintf(record, sizeof record, "{\"pid\":%ld,\"aliveAt\":%lld,\"checkMilliseconds\":%lld,\"holders\":%s}\n",
             pid, (long long)alive_at, (long long)TURN, holders);
    write_file(path, record);
    spisor_memory_release(path);
}

static char *read_processes_record(void) {
    char *path = processes_file(".json");
    size_t length = 0;
    char *text = (char *)spisor_read_file_reporting(path, &length, &(spisor_read_outcome){SPISOR_READ_FAILED});
    spisor_memory_release(path);
    return text;
}

/**
 * Another live process recorded as the loader's host with no holder, as a second in-process host in another
 * application's process would be recorded if in-process runs claimed the record, ends no in-process run: it
 * publishes no ready record, keeps ticking, and leaves the record as it found it.
 */
static void in_process_runs_serve_beside_another_host_and_leave_the_loaders_records_alone(void) {
    child_process other = start_sleeper();
    weivel_standard_host_outcome outcome;
    started_run run;
    char *before;
    char *after;
    cJSON *ready;
    CHECK(runtime_process_is_alive((double)other.pid));
    start_host_world("in-process");
    record_the_host(other.pid, NOW, "[]");
    before = read_processes_record();
    run = start_one(in_process_arguments, 3);
    outcome = outcome_of(&run, WEIVEL_STANDARD_HOST_ACTIVATED, NULL);
    CHECK(tell_started(&outcome, 1, NULL) == WEIVEL_STATUS_OK);
    ready = ready_record();
    CHECK(ready == NULL);
    cJSON_Delete(ready);
    expect_tick(NOW, NOW + TURN, "an in-process run's first tick");
    expect_tick(NOW + TURN, NOW + 2 * TURN, "an in-process run's second turn");
    expect_tick(NOW + 2 * TURN, NOW + 3 * TURN, "an in-process run's third turn");
    CHECK(run_record_alive_at() == (double)(NOW + 2 * TURN));
    after = read_processes_record();
    check_text(after, before, "the loader's holders record an in-process run leaves");
    spisor_memory_release(before);
    spisor_memory_release(after);
    release_run(&run);
    stop_expecting(0, "an in-process run");
    finish_base();
    kill_child(&other);
    release_child(&other);
}

/**
 * An in-process start hosts.start refuses, here for a loader registered at no version, leaves the loader's ready
 * record as it was, since a launch reading it waits on another process.
 */
static void a_refused_in_process_start_leaves_the_ready_record_alone(void) {
    char *path;
    char *before;
    char *after;
    size_t length = 0;
    started_run run;
    start_host_world("in-process-refused");
    path = processes_file(".ready.json");
    write_file(path, "{\"pid\":1,\"environment\":{}}\n");
    before = (char *)spisor_read_file_reporting(path, &length, &(spisor_read_outcome){SPISOR_READ_FAILED});
    write_in(state, "hosts/" LOADER ".json", "{\"id\": \"" LOADER "\", \"kind\": \"host\"}");
    run = start_with(in_process_arguments, 3);
    CHECK(run.status == WEIVEL_STATUS_REFUSED);
    after = (char *)spisor_read_file_reporting(path, &length, &(spisor_read_outcome){SPISOR_READ_FAILED});
    check_text(after, before, "the ready record a refused in-process start leaves");
    spisor_memory_release(before);
    spisor_memory_release(after);
    spisor_memory_release(path);
    release_run(&run);
    finish_base();
}

/** A recorded host whose process died, as a crashed or ended one leaves behind, never ends the next SERVE run. */
static void a_record_naming_a_dead_host_lets_a_serve_run_start_and_keep_ticking(void) {
    child_process dead = start_sleeper();
    weivel_standard_host_outcome outcome;
    started_run run;
    char holders[32];
    kill_child(&dead);
    CHECK(!runtime_process_is_alive((double)dead.pid));
    start_host_world("dead-host");
    snprintf(holders, sizeof holders, "[%d]", (int)current_process_id());
    record_the_host(dead.pid, NOW, holders);
    run = start_one(serve_arguments, 2);
    outcome = outcome_of(&run, WEIVEL_STANDARD_HOST_ACTIVATED, NULL);
    CHECK(tell_started(&outcome, 1, NULL) == WEIVEL_STATUS_OK);
    expect_tick(NOW, NOW + TURN, "a serve run beside a dead host's record");
    expect_tick(NOW + TURN, NOW + 2 * TURN, "its second turn");
    CHECK(holders_alive_at() == (double)(NOW + TURN));
    release_run(&run);
    stop_expecting(0, "a serve run that replaced a dead host");
    finish_base();
    release_child(&dead);
}

/**
 * A recorded host whose stamp is older than three of its checks never ends the next SERVE run, even while a live
 * process holds its pid, as an unrelated process given a dead host's pid would.
 */
static void a_record_naming_a_live_pid_with_a_stale_stamp_lets_a_serve_run_keep_ticking(void) {
    child_process unrelated = start_sleeper();
    weivel_standard_host_outcome outcome;
    started_run run;
    char holders[32];
    CHECK(runtime_process_is_alive((double)unrelated.pid));
    start_host_world("stale-host");
    snprintf(holders, sizeof holders, "[%d]", (int)current_process_id());
    record_the_host(unrelated.pid, NOW - 4 * TURN, holders);
    run = start_one(serve_arguments, 2);
    outcome = outcome_of(&run, WEIVEL_STANDARD_HOST_ACTIVATED, NULL);
    CHECK(tell_started(&outcome, 1, NULL) == WEIVEL_STATUS_OK);
    expect_tick(NOW, NOW + TURN, "a serve run beside a stale host's record");
    expect_tick(NOW + TURN, NOW + 2 * TURN, "its second turn");
    CHECK(holders_alive_at() == (double)(NOW + TURN));
    release_run(&run);
    stop_expecting(0, "a serve run that replaced a stale host");
    finish_base();
    kill_child(&unrelated);
    release_child(&unrelated);
}

int main(void) {
    spisor_json_use_library_allocator();
    a_serve_run_started_publishes_that_it_is_ready();
    a_front_door_serve_run_publishes_its_applications_variables();
    a_front_door_with_no_port_to_hand_publishes_a_refusal();
    a_proxy_the_standard_cannot_describe_publishes_a_refusal_naming_the_member();
    a_proxy_with_no_front_door_to_read_publishes_a_refusal();
    a_front_door_port_setting_that_is_not_set_publishes_a_refusal();
    a_front_door_whose_manifest_cannot_be_read_publishes_a_refusal();
    a_front_door_port_set_in_the_home_wins_over_the_portable_one();
    a_layer_not_holding_the_front_door_publishes_an_empty_environment();
    in_process_runs_serve_beside_another_host_and_leave_the_loaders_records_alone();
    a_record_naming_a_dead_host_lets_a_serve_run_start_and_keep_ticking();
    a_record_naming_a_live_pid_with_a_stale_stamp_lets_a_serve_run_keep_ticking();
    a_refused_in_process_start_leaves_the_ready_record_alone();
    a_plugin_not_activated_publishes_a_refusal_naming_it(
        WEIVEL_STANDARD_HOST_REFUSED, WEIVEL_STANDARD_HOST_ACTIVATED, "its scope was refused",
        "a is enabled but did not start: its scope was refused", "fix or disable a", "a refused plugin");
    a_plugin_not_activated_publishes_a_refusal_naming_it(
        WEIVEL_STANDARD_HOST_FAILED, WEIVEL_STANDARD_HOST_ACTIVATED, NULL,
        "a is enabled but did not start: it is not running", "fix or disable a", "a plugin that failed with no detail");
    a_plugin_not_activated_publishes_a_refusal_naming_it(
        WEIVEL_STANDARD_HOST_ACTIVATED, WEIVEL_STANDARD_HOST_FAILED, "it threw",
        "b is enabled but did not start: it threw", "fix or disable b", "a later plugin that failed");
    an_action_runs_on_when_another_plugin_was_not_activated();
    outcomes_that_are_not_the_plans_are_malformed();
    a_second_started_is_refused_and_fails_the_run();
    an_action_answers_its_exit_code_as_it_ended();
    finished_is_refused_in_a_serve_run_and_before_started();
    a_tick_acts_on_each_timer_when_it_is_due();
    a_late_tick_runs_each_timer_once();
    a_tick_at_the_epoch_reads_no_clock();
    a_tick_far_behind_the_clock_keeps_its_run_alive_through_its_check();
    a_serve_run_no_holder_keeps_ends_with_exit_code_zero();
    failed_prunes_end_the_run_only_once_enough_fail_in_a_row();
    a_now_no_check_can_carry_fails_the_run();
    return check_summary("host_progress_test");
}
