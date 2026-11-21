#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "host_run_progress.h"
#include "allocation.h"

#include <math.h>
#include <string.h>

#include "cache_refusal.h"
#include "catalog_time.h"
#include "definition_support.h"
#include "platform_key.h"
#include "processes.h"
#include "processes_ready.h"
#include "spisor/definition.h"
#include "updates.h"

#if defined(_WIN32)
#include <process.h>
#define current_process_id() _getpid()
#else
#include <unistd.h>
#define current_process_id() getpid()
#endif

#define PLUGIN_FIX "turn the plugin off in the loader's settings, or install it again, then start the loader again"
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

/** The first outcome that is not ACTIVATED, with the id of its plugin, or NULL when every plugin was activated. */
static const weivel_standard_host_outcome *first_not_activated(const runtime_host_run *run,
                                                               const weivel_standard_host_outcome *outcomes,
                                                               const char **plugin_id) {
    const cJSON *plugin;
    size_t index = 0;
    cJSON_ArrayForEach(plugin, member(run->located, "located")) {
        if (outcomes[index].state != WEIVEL_STANDARD_HOST_ACTIVATED) {
            *plugin_id = text_of(plugin, "id");
            return &outcomes[index];
        }
        index++;
    }
    return NULL;
}

/**
 * Refuses the run for the plugin that was not activated, and for a SERVE run publishes why to the loader's ready
 * record, so a launch waiting on it reads the cause rather than timing out.
 *
 * @implNote The publication is best effort, as a refused start's is: the run is refused either way.
 */
static spisor_status refuse_plugin(const runtime_host_run *run, const weivel_standard_host_outcome *outcome,
                                   const char *plugin_id, spisor_refusal *refusal) {
    const char *how = outcome->state == WEIVEL_STANDARD_HOST_REFUSED ? " was refused" : " failed to activate";
    int has_detail = outcome->detail != NULL && outcome->detail[0] != '\0';
    const char *parts[7] = {"the plugin ", plugin_id, " of ", run->host_id, how, has_detail ? ": " : "",
                            has_detail ? outcome->detail : ""};
    spisor_status status = refuse_joined(refusal, parts, 7, PLUGIN_FIX);
    cJSON *published;
    if (status == SPISOR_REFUSED && run->mode == WEIVEL_STANDARD_HOST_SERVE) {
        published = runtime_refusal(refusal->detail, PLUGIN_FIX);
        if (published != NULL) {
            runtime_publish_ready_refusal(state_of(run), run->host_id, published);
        }
        cJSON_Delete(published);
    }
    return status;
}

/** Publishes that the SERVE run is ready, with the empty environment a loader with no session service gives. */
static spisor_status publish_ready(const runtime_host_run *run, spisor_refusal *refusal) {
    cJSON *environment = cJSON_CreateObject();
    cJSON *json_refusal = NULL;
    runtime_processes_publication publication = {state_of(run), run->host_id, (double)current_process_id(),
                                                 environment, NULL, NULL};
    int published = environment != NULL && runtime_processes_ready(&publication, &json_refusal);
    cJSON_Delete(environment);
    return published ? SPISOR_OK : runtime_refuse_json(refusal, json_refusal);
}

void runtime_host_run_fail(runtime_host_run *run) {
    run->phase = RUNTIME_HOST_ENDED;
    run->exit_code = RUNTIME_HOST_EXIT_FAILED;
}

static spisor_status failed_unless_ok(runtime_host_run *run, spisor_status status) {
    if (status != SPISOR_OK) {
        runtime_host_run_fail(run);
    }
    return status;
}

spisor_status runtime_host_run_started(runtime_host_run *run, const weivel_standard_host_outcome *outcomes,
                                       size_t outcome_count, spisor_refusal *refusal) {
    const weivel_standard_host_outcome *refused;
    const char *plugin_id = NULL;
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
    refused = first_not_activated(run, outcomes, &plugin_id);
    if (refused != NULL) {
        return failed_unless_ok(run, refuse_plugin(run, refused, plugin_id, refusal));
    }
    status = run->mode == WEIVEL_STANDARD_HOST_SERVE ? publish_ready(run, refusal) : SPISOR_OK;
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

/** One turn: the run record stamped and the holders pruned, ending the run when they say so. */
static spisor_status take_turn(runtime_host_run *run, int64_t now, int64_t interval, spisor_refusal *refusal) {
    cJSON *failure = NULL;
    int unheld = 0;
    spisor_status status = stamp_and_prune(run, now, interval, &unheld, &failure);
    int64_t allowed = timer_number(run, "pruneFailuresBeforeExit");
    const char *message = text_of(failure, "message");
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

/** One updates.check at `now`, its problems the versions the run could not locate; a refused check changes nothing. */
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
    return run->phase == RUNTIME_HOST_ENDED ? status : failed_unless_ok(run, status);
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
