#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "host_run_progress.h"
#include "allocation.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cache_refusal.h"
#include "catalog_time.h"
#include "definition_support.h"
#include "log.h"
#include "node_json_file.h"
#include "platform_key.h"
#include "processes.h"
#include "processes_ready.h"
#include "scope_host.h"
#include "spisor/definition.h"
#include "updates.h"

#if defined(_WIN32)
#include <process.h>
#define current_process_id() _getpid()
#else
#include <unistd.h>
#define current_process_id() getpid()
#endif

#define FRONT_DOOR "front-door"
#define UNHELD_FIX "hold the loader again to run it"
#define ORDER_FIX "call started once after start, tick in a SERVE run after started answered OK, and finished once"

static const cJSON *member(const cJSON *object, const char *name) {
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

static const char *text_of(const cJSON *object, const char *name) {
    return cJSON_GetStringValue(member(object, name));
}

static const cJSON *plan_of(const runtime_host_run *run) {
    return member(run->started, "plan");
}

static const char *state_of(const runtime_host_run *run) {
    return text_of(member(run->started, "places"), "state");
}

/** Refuses with the message `parts` join, so a plugin's detail is never cut short. */
static spisor_status refuse_joined(spisor_refusal *refusal, const char *const *parts, size_t count, const char *fix) {
    char *message = runtime_cache_text(parts, count);
    spisor_status status = message == NULL ? SPISOR_OUT_OF_MEMORY
                                           : spisor_refuse_as(refusal, SPISOR_REFUSED, message, fix);
    spisor_memory_release(message);
    return status;
}

static spisor_status refuse_out_of_order(spisor_refusal *refusal, const char *detail) {
    return spisor_refuse_as(refusal, SPISOR_REFUSED, detail, ORDER_FIX);
}

static int is_plugin_state(uint32_t state) {
    return state == WEIVEL_STANDARD_HOST_ACTIVATED || state == WEIVEL_STANDARD_HOST_REFUSED ||
           state == WEIVEL_STANDARD_HOST_FAILED;
}

/** Whether the outcomes are one per plugin of the plan, each naming that plugin's manifest and a plugin state. */
static int are_plan_outcomes(const runtime_host_run *run, const weivel_standard_host_outcome *outcomes, size_t count) {
    const cJSON *located = member(run->located, "located");
    const cJSON *plugin;
    size_t index = 0;
    if (count != (size_t)cJSON_GetArraySize(located)) {
        return 0;
    }
    cJSON_ArrayForEach(plugin, located) {
        const weivel_standard_host_outcome *outcome = &outcomes[index++];
        if (outcome->manifest == NULL || strcmp(outcome->manifest, text_of(plugin, "manifest")) != 0 ||
            !is_plugin_state(outcome->state)) {
            return 0;
        }
    }
    return 1;
}

/** Whether a manifest declares the front-door capability, the one a session host serves its application through. */
static int declares_front_door(const cJSON *manifest) {
    const cJSON *capability;
    cJSON_ArrayForEach(capability, member(manifest, "capabilities")) {
        if (cJSON_IsString(capability) && strcmp(capability->valuestring, FRONT_DOOR) == 0) {
            return 1;
        }
    }
    return 0;
}

/**
 * The first plugin that was not activated and whose manifest declares the front door, its id in `*plugin_id`, or NULL
 * for none; `*out_of_memory` set when memory ran out reading a manifest.
 *
 * @implNote Only a manifest that reads can be seen declaring the capability; a plugin whose manifest does not was
 * refused like any other, as the archived host's was.
 */
static const weivel_standard_host_outcome *stopped_front_door(const runtime_host_run *run,
                                                              const weivel_standard_host_outcome *outcomes,
                                                              const char **plugin_id, int *out_of_memory) {
    const cJSON *plugin;
    size_t index = 0;
    *out_of_memory = 0;
    cJSON_ArrayForEach(plugin, member(run->located, "located")) {
        const weivel_standard_host_outcome *outcome = &outcomes[index++];
        cJSON *manifest = outcome->state == WEIVEL_STANDARD_HOST_ACTIVATED
                              ? NULL
                              : runtime_read_node_json(text_of(plugin, "manifest"), out_of_memory);
        int front_door = declares_front_door(manifest);
        cJSON_Delete(manifest);
        if (*out_of_memory) {
            return NULL;
        }
        if (front_door) {
            *plugin_id = text_of(plugin, "id");
            return outcome;
        }
    }
    return NULL;
}

/** Publishes to the loader's ready record that the SERVE run is ready with `environment`, or that it is `refused`. */
static spisor_status publish(const runtime_host_run *run, const cJSON *environment, const char *refused,
                             const char *fix, spisor_refusal *refusal) {
    cJSON *json_refusal = NULL;
    runtime_processes_publication publication = {state_of(run), run->host_id, (double)current_process_id(),
                                                 environment, refused, fix};
    return runtime_processes_ready(&publication, &json_refusal) ? SPISOR_OK
                                                                : runtime_refuse_json(refusal, json_refusal);
}

/**
 * Publishes that the front door `plugin_id` did not start, as DECISIONS rules for an enabled front door that is
 * quarantined or not running: a refusal naming it, never an empty environment, since silently bypassing the
 * configured providers is the switch the user must not get.
 */
static spisor_status publish_stopped_front_door(const runtime_host_run *run,
                                                const weivel_standard_host_outcome *outcome, const char *plugin_id,
                                                spisor_refusal *refusal) {
    int has_detail = outcome->detail != NULL && outcome->detail[0] != '\0';
    const char *parts[4] = {"the front door ", plugin_id, " is enabled but did not start: ",
                            has_detail ? outcome->detail : "it is not running"};
    const char *fix_parts[2] = {"fix or disable ", plugin_id};
    char *refused = runtime_cache_text(parts, 4);
    char *fix = refused == NULL ? NULL : runtime_cache_text(fix_parts, 2);
    spisor_status status = fix == NULL ? SPISOR_OUT_OF_MEMORY : publish(run, NULL, refused, fix, refusal);
    spisor_memory_release(fix);
    spisor_memory_release(refused);
    return status;
}

/**
 * Publishes a SERVE run's ready record: ready, with the empty environment a loader with no session service gives,
 * unless its front door did not start.
 */
static spisor_status publish_serving(const runtime_host_run *run, const weivel_standard_host_outcome *outcomes,
                                     spisor_refusal *refusal) {
    const char *plugin_id = NULL;
    int out_of_memory = 0;
    const weivel_standard_host_outcome *front_door = stopped_front_door(run, outcomes, &plugin_id, &out_of_memory);
    cJSON *environment;
    spisor_status status;
    if (out_of_memory) {
        return SPISOR_OUT_OF_MEMORY;
    }
    if (front_door != NULL) {
        return publish_stopped_front_door(run, front_door, plugin_id, refusal);
    }
    environment = cJSON_CreateObject();
    status = environment == NULL ? SPISOR_OUT_OF_MEMORY : publish(run, environment, NULL, NULL, refusal);
    cJSON_Delete(environment);
    return status;
}

void runtime_host_run_fail(runtime_host_run *run) {
    if (run->phase != RUNTIME_HOST_ENDED) {
        run->exit_code = RUNTIME_HOST_EXIT_FAILED;
    }
    run->phase = RUNTIME_HOST_ENDED;
}

static spisor_status failed_unless_ok(runtime_host_run *run, spisor_status status) {
    if (status != SPISOR_OK) {
        runtime_host_run_fail(run);
    }
    return status;
}

spisor_status runtime_host_run_started(runtime_host_run *run, const weivel_standard_host_outcome *outcomes,
                                       size_t outcome_count, spisor_refusal *refusal) {
    spisor_status status;
    if (run->phase != RUNTIME_HOST_PLANNED) {
        return failed_unless_ok(run, refuse_out_of_order(refusal, "host's started was called a second time"));
    }
    if (!are_plan_outcomes(run, outcomes, outcome_count)) {
        status = spisor_refuse_as(refusal, SPISOR_MALFORMED,
                                  "host's started was not told one outcome per plugin of the plan, in its order",
                                  "pass each plugin's outcome, naming the manifest the plan named and its state");
        return failed_unless_ok(run, status);
    }
    status = run->mode == WEIVEL_STANDARD_HOST_SERVE ? publish_serving(run, outcomes, refusal) : SPISOR_OK;
    if (status == SPISOR_OK) {
        run->phase = RUNTIME_HOST_RUNNING;
    }
    return failed_unless_ok(run, status);
}

/** A number of hosts.start's timers, or 0 when it answered none that is 1 or more. */
static int64_t timer_number(const runtime_host_run *run, const char *name) {
    const cJSON *interval = member(member(run->started, "timers"), name);
    double milliseconds = cJSON_IsNumber(interval) ? interval->valuedouble : 0;
    return milliseconds >= 1 && milliseconds <= 9007199254740991.0 ? (int64_t)floor(milliseconds) : 0;
}

static int64_t later_by(int64_t now, int64_t interval) {
    return now > INT64_MAX - interval ? INT64_MAX : now + interval;
}

static int is_due(const runtime_host_run *run, int64_t interval, int64_t due, int64_t now) {
    return interval > 0 && (!run->timers.ticked || now >= due);
}

/** Ends a SERVE run on its holders' word: REFUSED, so the program stops, with the exit code left 0. */
static spisor_status end_unheld(runtime_host_run *run, const char *const *parts, size_t count,
                                spisor_refusal *refusal) {
    spisor_status status = refuse_joined(refusal, parts, count, UNHELD_FIX);
    if (status == SPISOR_REFUSED) {
        run->phase = RUNTIME_HOST_ENDED;
    }
    return status;
}

/** Stamps the run record and prunes the holders; `*unheld` the prune's exit, `*failure` the refusal of either. */
static spisor_status stamp_and_prune(const runtime_host_run *run, int64_t now, int64_t interval, int *unheld,
                                     cJSON **failure) {
    const cJSON *plan = plan_of(run);
    double at = (double)now;
    runtime_processes_record_request record = {state_of(run), run->host_id, member(plan, "run"), (double)interval,
                                               text_of(plan, "runRecord"), &at};
    runtime_processes_prune_request prune = {state_of(run), run->host_id, (double)current_process_id(), &at,
                                             (double)interval};
    cJSON *stamped = runtime_processes_record_run(&record, failure);
    cJSON *pruned = stamped == NULL ? NULL : runtime_processes_prune(&prune, failure);
    spisor_status status = pruned == NULL && *failure == NULL ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
    *unheld = cJSON_IsTrue(member(pruned, "exit"));
    cJSON_Delete(stamped);
    cJSON_Delete(pruned);
    return status;
}

/**
 * Writes a warning about the run to the loader's own log, under the loader's id at `now`, as the archived host wrote
 * one to stderr, printing the console line runtime's log answers.
 *
 * @implNote Best effort, and synchronous, so it costs the tick what one log write costs: a line that cannot be written,
 * memory running out included, leaves the tick's answer as it was, since what it reports already happened.
 */
static void log_warning(const runtime_host_run *run, int64_t now, const char *const *parts, size_t count) {
    runtime_owned_scope_host host;
    runtime_log_request entry;
    runtime_log_console console = {NULL, NULL};
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    char at[RUNTIME_CATALOG_TIME_SIZE];
    char *message;
    if (!runtime_catalog_format_time((double)now, at) || runtime_scopes_host(&host) != SPISOR_OK) {
        return;
    }
    message = runtime_cache_text(parts, count);
    memset(&entry, 0, sizeof entry);
    entry.home = text_of(member(run->started, "identity"), "home");
    entry.portable = text_of(member(run->started, "places"), "portable");
    entry.plugin = run->host_id;
    entry.start = host.start;
    entry.time = at;
    entry.message = message;
    entry.level = RUNTIME_LOG_WARN;
    entry.no_color = cJSON_IsTrue(member(run->started, "noColor"));
    if (message != NULL && runtime_write_log(&entry, &console, &refusal) == SPISOR_OK && console.line != NULL) {
        fprintf(stderr, "%s\n", console.line);
    }
    runtime_release_storage(console.storage);
    spisor_refusal_release(&refusal);
    spisor_memory_release(message);
    runtime_scope_host_free(&host);
}

/** One turn: the run record stamped and the holders pruned, ending the run when they say so. */
static spisor_status take_turn(runtime_host_run *run, int64_t now, int64_t interval, spisor_refusal *refusal) {
    cJSON *failure = NULL;
    int unheld = 0;
    spisor_status status = stamp_and_prune(run, now, interval, &unheld, &failure);
    int64_t allowed = timer_number(run, "pruneFailuresBeforeExit");
    const char *message = text_of(failure, "message");
    const char *warning[4] = {"spisor could not check the holders of ", run->host_id, ": ",
                              message == NULL ? "its holders file could not be used" : message};
    if (failure != NULL) {
        log_warning(run, now, warning, 4);
    }
    if (status == SPISOR_OK && failure != NULL && ++run->timers.failed_turns >= (uint64_t)(allowed < 1 ? 1 : allowed)) {
        const char *parts[4] = {"the holders of ", run->host_id, " could not be checked, so its run ends: ",
                                message == NULL ? "" : message};
        status = end_unheld(run, parts, 4, refusal);
    } else if (status == SPISOR_OK && failure == NULL) {
        const char *parts[3] = {"no holder of ", run->host_id, " is alive, or another process runs it, so it ends"};
        run->timers.failed_turns = 0;
        status = unheld ? end_unheld(run, parts, 3, refusal) : SPISOR_OK;
    }
    cJSON_Delete(failure);
    return status;
}

static const char *text_or_empty(const cJSON *object, const char *name) {
    const char *text = text_of(object, name);
    return text == NULL ? "" : text;
}

/** Logs the check's refusal, or each version its answer says it refused. */
static void log_check(const runtime_host_run *run, int64_t now, const cJSON *checked, const cJSON *failure) {
    const cJSON *refused;
    if (failure != NULL) {
        const char *parts[4] = {"spisor could not check for updates of ", run->host_id, ": ",
                                text_or_empty(failure, "message")};
        log_warning(run, now, parts, 4);
    }
    cJSON_ArrayForEach(refused, member(checked, "refused")) {
        const char *parts[6] = {"spisor could not update ", text_or_empty(refused, "id"), ": ",
                                text_or_empty(refused, "message"), "; fix: ", text_or_empty(refused, "fix")};
        log_warning(run, now, parts, 6);
    }
}

/**
 * One updates.check at `now`, its problems the versions the run could not locate; a refused check changes nothing but
 * the loader's log, which it and each version it refused are written to.
 */
static spisor_status check_updates(const runtime_host_run *run, int64_t now, spisor_refusal *refusal) {
    char at[RUNTIME_CATALOG_TIME_SIZE];
    const cJSON *places = member(run->started, "places");
    runtime_updates_check_request request = {{state_of(run), text_of(places, "cache"), run->host_id,
                                              runtime_platform_key()},
                                             at, member(run->started, "settings"), NULL,
                                             member(run->located, "problems")};
    cJSON *failure = NULL;
    cJSON *checked;
    spisor_status status;
    if (!runtime_catalog_format_time((double)now, at)) {
        return spisor_refuse_as(refusal, SPISOR_REFUSED, "host's tick was given a now outside the years 0 to 9999",
                                "pass the milliseconds since the Unix epoch the program's clock reads");
    }
    checked = runtime_updates_check(&request, &failure);
    status = checked == NULL && failure == NULL ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
    log_check(run, now, checked, failure);
    cJSON_Delete(failure);
    cJSON_Delete(checked);
    return status;
}

static spisor_status refuse_untickable(const runtime_host_run *run, spisor_refusal *refusal) {
    return refuse_out_of_order(refusal, run->mode != WEIVEL_STANDARD_HOST_SERVE
                                            ? "host's tick was called in an ACTION run, which never ticks"
                                            : "host's tick was called before started answered OK, or after the run "
                                              "ended");
}

static void answer_next(const runtime_host_run *run, int64_t turn, int64_t check, runtime_host_next_tick *next) {
    next->has_next = turn > 0 || check > 0;
    next->next = turn > 0 && (check <= 0 || run->timers.next_turn <= run->timers.next_check) ? run->timers.next_turn
                                                                                              : run->timers.next_check;
    if (!next->has_next) {
        next->next = 0;
    }
}

spisor_status runtime_host_run_tick(runtime_host_run *run, int64_t now, runtime_host_next_tick *next,
                                    spisor_refusal *refusal) {
    int64_t turn = timer_number(run, "recordRunMilliseconds");
    int64_t check = timer_number(run, "updateCheckMilliseconds");
    int check_due = is_due(run, check, run->timers.next_check, now);
    spisor_status status = SPISOR_OK;
    memset(next, 0, sizeof *next);
    if (run->mode != WEIVEL_STANDARD_HOST_SERVE || run->phase != RUNTIME_HOST_RUNNING) {
        return failed_unless_ok(run, refuse_untickable(run, refusal));
    }
    if (is_due(run, turn, run->timers.next_turn, now)) {
        status = take_turn(run, now, turn, refusal);
        run->timers.next_turn = later_by(now, turn);
    }
    if (status == SPISOR_OK && check_due) {
        status = check_updates(run, now, refusal);
        run->timers.next_check = later_by(now, check);
    }
    run->timers.ticked = 1;
    if (status == SPISOR_OK) {
        answer_next(run, turn, check, next);
    }
    return failed_unless_ok(run, status);
}

spisor_status runtime_host_run_finished(runtime_host_run *run, const char *failure, spisor_refusal *refusal) {
    if (run->mode != WEIVEL_STANDARD_HOST_ACTION) {
        return failed_unless_ok(run, refuse_out_of_order(refusal, "host's finished was called in a SERVE run, which "
                                                                  "runs no action"));
    }
    if (run->phase != RUNTIME_HOST_RUNNING) {
        return failed_unless_ok(run, refuse_out_of_order(refusal, "host's finished was called before started "
                                                                  "answered OK, or a second time"));
    }
    run->phase = RUNTIME_HOST_ENDED;
    run->exit_code = failure == NULL ? 0 : RUNTIME_HOST_EXIT_FAILED;
    return SPISOR_OK;
}
