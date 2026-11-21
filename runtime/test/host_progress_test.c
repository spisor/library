#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "host_fixture.h"

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

static const char *const serve_arguments[1] = {LOADER};
static const char *const action_arguments[5] = {LOADER, "action", "a", "greet", "{}"};

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

/** The ready record the loader's last run published, parsed, or NULL when there is none. */
static cJSON *ready_record(void) {
    char *path = processes_file(".ready.json");
    size_t length = 0;
    char *text = (char *)spisor_read_file_reporting(path, &length, &(spisor_read_outcome){SPISOR_READ_FAILED});
    cJSON *record = text == NULL ? NULL : spisor_parse_json_reporting((const uint8_t *)text, length, &(int){0});
    spisor_memory_release(text);
    spisor_memory_release(path);
    return record;
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

/**
 * When the loader's one run record says its run was last alive, which a tick's turn stamps with its now.
 *
 * @implNote Read only after a tick that ran no update check: the check's sweep reads the clock, and deletes a record
 * stamped years before it, as NOW is, until the next turn stamps it again.
 */
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
    start_host_world(label);
    hold_the_loader();
    run = start_one(serve_arguments, 1);
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

static void a_plugin_not_activated_refuses_the_run_and_is_published(uint32_t plugin_state, const char *detail,
                                                                     const char *expected, const char *label) {
    weivel_standard_service_refusal refusal;
    weivel_standard_host_outcome outcome;
    started_run run;
    cJSON *record;
    start_host_world(label);
    run = start_one(serve_arguments, 1);
    outcome = outcome_of(&run, plugin_state, detail);
    CHECK(tell_started(&outcome, 1, &refusal) == WEIVEL_STATUS_REFUSED);
    expect_detail(&refusal, expected, label);
    record = ready_record();
    check_text(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(record, "refused")), refusal.detail,
               "the published refusal");
    CHECK(cJSON_GetObjectItemCaseSensitive(record, "environment") == NULL);
    cJSON_Delete(record);
    release_refusal(&refusal);
    expect_tick_refused(NOW, "before started answered OK", "a tick after a refused started");
    release_run(&run);
    stop_expecting(RUNTIME_HOST_EXIT_FAILED, label);
    finish_base();
}

static void outcomes_that_are_not_the_plans_are_malformed(void) {
    weivel_standard_host_outcome outcomes[2];
    started_run run;
    const char *labels[5] = {"no outcome", "two outcomes", "another manifest", "no state", "an unknown state"};
    for (size_t index = 0; index < 5; index++) {
        start_host_world("malformed-outcomes");
        run = start_one(serve_arguments, 1);
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
    run = start_one(action_arguments, 5);
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
    run = start_one(action_arguments, 5);
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
    run = start_one(serve_arguments, 1);
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

static void failed_prunes_end_the_run_only_once_enough_fail_in_a_row(void) {
    started_run run = serving("prune-failures");
    int64_t now = NOW;
    break_the_prune(1);
    for (int turn = 1; turn < FAILURES_BEFORE_EXIT; turn++, now += TURN) {
        expect_tick(now, now + TURN, "a failed turn");
    }
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

int main(void) {
    spisor_json_use_library_allocator();
    a_serve_run_started_publishes_that_it_is_ready();
    a_plugin_not_activated_refuses_the_run_and_is_published(WEIVEL_STANDARD_HOST_REFUSED, "its scope was refused",
                                                            "the plugin a of " LOADER
                                                            " was refused: its scope was refused",
                                                            "a refused plugin");
    a_plugin_not_activated_refuses_the_run_and_is_published(WEIVEL_STANDARD_HOST_FAILED, NULL,
                                                            "the plugin a of " LOADER " failed to activate",
                                                            "a failed plugin");
    outcomes_that_are_not_the_plans_are_malformed();
    a_second_started_is_refused_and_fails_the_run();
    an_action_answers_its_exit_code_as_it_ended();
    finished_is_refused_in_a_serve_run_and_before_started();
    a_tick_acts_on_each_timer_when_it_is_due();
    a_late_tick_runs_each_timer_once();
    a_tick_at_the_epoch_reads_no_clock();
    a_serve_run_no_holder_keeps_ends_with_exit_code_zero();
    failed_prunes_end_the_run_only_once_enough_fail_in_a_row();
    a_now_no_check_can_carry_fails_the_run();
    return check_summary("host_progress_test");
}
