#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog_call.h"
#include "check.h"
#include "child_process.h"
#include "directory_tree.h"
#include "file_times.h"
#include "holders.h"
#include "holders_lock.h"
#include "process_alive.h"
#include "processes.h"
#include "scratch_directory.h"
#include "spisor/file.h"

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#define current_process_id() _getpid()
#else
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#define current_process_id() getpid()
#endif

#define HOST "background-fixture"
#define INTERVAL 750.0
#define INTERVAL_TEXT "750"
#define ALLOWANCE (4 * INTERVAL)
#define FIXTURE RUNTIME_BACKGROUND_FIXTURE_PATH

static char state[1024];
static double detached[64];
static size_t detached_count = 0;

static char *joined(const char *base, const char *rest) {
    size_t length = strlen(base) + strlen(rest) + 2;
    char *path = malloc(length);
    snprintf(path, length, "%s/%s", base, rest);
    return path;
}

static void fresh_state(const char *name) {
    snprintf(state, sizeof state, "%s/processes-%s-%d", RUNTIME_SCRATCH_PATH, name, (int)current_process_id());
    start_empty_directory(state);
}

static void write_text(const char *rest, const char *text) {
    char *path = joined(state, rest);
    char *slash = strrchr(path, '/');
    *slash = 0;
    CHECK(spisor_make_directories(path) == 1);
    *slash = '/';
    CHECK(spisor_replace_file(path, (const uint8_t *)text, strlen(text)) == 1);
    free(path);
}

static char *read_text(const char *rest) {
    char *path = joined(state, rest);
    size_t length = 0;
    char *text = (char *)spisor_read_file(path, &length);
    free(path);
    return text;
}

static int present(const char *rest) {
    char *path = joined(state, rest);
    int found = spisor_file_exists(path);
    free(path);
    return found;
}

static double self_pid(void) {
    return (double)current_process_id();
}

static void remember_detached(double pid) {
    if (detached_count < 64) {
        detached[detached_count++] = pid;
    }
}

/** Kills every host a case started and left, as the TypeScript afterEach did. */
static void finish_case(void) {
    for (size_t index = 0; index < detached_count; index++) {
        kill_pid((long)detached[index]);
    }
    detached_count = 0;
    runtime_remove_tree(state);
}

/* the holders record, as JSON.stringify writes it */

static void write_record(const char *pid, const char *alive_at, const char *check, const char *holders) {
    char text[512];
    snprintf(text, sizeof text, "{\"pid\":%s,\"aliveAt\":%s,\"checkMilliseconds\":%s,\"holders\":%s}\n", pid, alive_at,
             check, holders);
    write_text("processes/" HOST ".json", text);
}

static char *number_text(double value) {
    static char texts[8][32];
    static int next = 0;
    char *text = texts[next++ % 8];
    snprintf(text, 32, "%.0f", value);
    return text;
}

static runtime_holders_record read_record(void) {
    char *path = joined(state, "processes/" HOST ".json");
    runtime_holders_record record = runtime_no_holders();
    CHECK(runtime_read_holders(path, &record) == 1);
    free(path);
    return record;
}

static int holders_are(const runtime_holders_record *record, const double *expected, size_t count) {
    if (record->count != count) {
        return 0;
    }
    for (size_t index = 0; index < count; index++) {
        if (record->holders[index] != expected[index]) {
            return 0;
        }
    }
    return 1;
}

static int holders_file_present(void) {
    return present("processes/" HOST ".json");
}

/* calls */

static cJSON *start_of(const char *const *arguments, size_t count) {
    cJSON *start = cJSON_CreateObject();
    cJSON_AddStringToObject(start, "executable", FIXTURE);
    cJSON *list = cJSON_AddArrayToObject(start, "arguments");
    for (size_t index = 0; index < count; index++) {
        cJSON_AddItemToArray(list, cJSON_CreateString(arguments[index]));
    }
    return start;
}

static cJSON *background_start(void) {
    const char *arguments[4] = {"background", state, HOST, INTERVAL_TEXT};
    return start_of(arguments, 4);
}

static cJSON *sleep_start(void) {
    const char *arguments[1] = {"sleep"};
    return start_of(arguments, 1);
}

static cJSON *host_argument(const char *host_id) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "state", state);
    cJSON_AddStringToObject(argument, "hostId", host_id);
    return argument;
}

static catalog_answer hold(double holder, cJSON *start) {
    cJSON *argument = host_argument(HOST);
    cJSON_AddNumberToObject(argument, "holder", holder);
    cJSON_AddItemToObject(argument, "start", start);
    return call_catalog(runtime_processes_hold, argument);
}

static catalog_answer release(double holder) {
    cJSON *argument = host_argument(HOST);
    cJSON_AddNumberToObject(argument, "holder", holder);
    return call_catalog(runtime_processes_release, argument);
}

static catalog_answer prune(double self, double now) {
    cJSON *argument = host_argument(HOST);
    cJSON_AddNumberToObject(argument, "self", self);
    cJSON_AddNumberToObject(argument, "now", now);
    cJSON_AddNumberToObject(argument, "checkMilliseconds", INTERVAL);
    return call_catalog(runtime_processes_prune, argument);
}

static double answered_pid(const catalog_answer *answer) {
    const cJSON *pid = cJSON_GetObjectItemCaseSensitive(answer->tree, "pid");
    return cJSON_IsNumber(pid) ? pid->valuedouble : NAN;
}

static int answered_flag(const catalog_answer *answer, const char *member) {
    const cJSON *flag = cJSON_GetObjectItemCaseSensitive(answer->tree, member);
    return cJSON_IsBool(flag) ? cJSON_IsTrue(flag) : -1;
}

/** Holds for this process through start, expecting a new host, and answers its pid. */
static double hold_starting(cJSON *start) {
    catalog_answer answer = hold(self_pid(), start);
    CHECK(answer.status == 0);
    CHECK(answered_flag(&answer, "started") == 1);
    double pid = answered_pid(&answer);
    remember_detached(pid);
    cJSON_Delete(answer.tree);
    return pid;
}

static child_process start_sleeper(void) {
    const char *arguments[1] = {"sleep"};
    return start_child(FIXTURE, arguments, 1, 0, NULL);
}

static child_process exited_child(void) {
    child_process child = start_sleeper();
    kill_child(&child);
    return child;
}

/* holders.test.ts: the holders file */

static void it_reads_back_what_was_written_and_reads_as_empty_when_there_is_none(void) {
    fresh_state("read-back");
    runtime_holders_record none = read_record();
    CHECK(isnan(none.pid) && isnan(none.alive_at) && isnan(none.check_milliseconds) && none.count == 0);
    double holders[2] = {7, 9};
    runtime_holders_record record = {41, 1000, 200, holders, 2};
    char *path = joined(state, "processes/" HOST ".json");
    CHECK(spisor_make_directories(state) == 1);
    CHECK(runtime_write_holders(path, &record) == 1);
    free(path);
    runtime_holders_record back = read_record();
    CHECK(back.pid == 41 && back.alive_at == 1000 && back.check_milliseconds == 200 && holders_are(&back, holders, 2));
    runtime_free_holders(&back);
    char *text = read_text("processes/" HOST ".json");
    check_text(text, "{\"pid\":41,\"aliveAt\":1000,\"checkMilliseconds\":200,\"holders\":[7,9]}\n", "holders file");
    free(text);
    finish_case();
}

static void it_removes_the_file_once_it_records_neither_a_process_nor_a_holder(void) {
    fresh_state("removes");
    char *path = joined(state, "processes/" HOST ".json");
    runtime_holders_record only_pid = runtime_no_holders();
    only_pid.pid = 41;
    CHECK(runtime_write_holders(path, &only_pid) == 1);
    char *text = read_text("processes/" HOST ".json");
    check_text(text, "{\"pid\":41,\"aliveAt\":null,\"checkMilliseconds\":null,\"holders\":[]}\n", "pid only");
    free(text);
    runtime_holders_record none = runtime_no_holders();
    CHECK(runtime_write_holders(path, &none) == 1);
    CHECK(!holders_file_present());
    free(path);
    finish_case();
}

static void it_reads_a_file_that_is_not_a_holders_record_as_empty(void) {
    fresh_state("not-a-record");
    const char *texts[4] = {"{ not json", "{\"pid\":41,\"holders\":[0]}", "{\"pid\":41,\"holders\":[1.5]}", "[1]"};
    for (size_t index = 0; index < 4; index++) {
        write_text("processes/" HOST ".json", texts[index]);
        runtime_holders_record record = read_record();
        CHECK(isnan(record.pid) && record.count == 0);
    }
    write_record("-3", "\"soon\"", "null", "[5]");
    runtime_holders_record kept = read_record();
    double five[1] = {5};
    CHECK(isnan(kept.pid) && isnan(kept.alive_at) && holders_are(&kept, five, 1));
    runtime_free_holders(&kept);
    finish_case();
}

static void it_refuses_a_host_id_that_would_place_the_file_outside_the_processes_folder(void) {
    fresh_state("escape");
    char *problem = NULL;
    CHECK(runtime_holders_path(state, "../escape", ".json", &problem) == NULL);
    CHECK(problem != NULL && strstr(problem, "would record its processes outside") != NULL);
    free(problem);
    cJSON *argument = host_argument("../escape");
    cJSON_AddNumberToObject(argument, "holder", self_pid());
    cJSON_AddItemToObject(argument, "start", sleep_start());
    catalog_answer answer = call_catalog(runtime_processes_hold, argument);
    CHECK(answer.status != 0);
    const char *message = text_at(answer.tree, "message");
    CHECK(message != NULL && strstr(message, "would record its processes outside") != NULL);
    check_text(text_at(answer.tree, "fix"), "give the host an id that is a plain file name", "escape fix");
    cJSON_Delete(answer.tree);
    finish_case();
}

/* holders.test.ts: the lock */

static char *lock_path(void) {
    return joined(state, "processes/" HOST ".lock");
}

static runtime_holders_guard *take_lock(void) {
    char *folder = joined(state, "processes");
    CHECK(spisor_make_directories(folder) == 1);
    free(folder);
    char *path = lock_path();
    runtime_holders_guard *guard = runtime_lock_holders(path);
    free(path);
    return guard;
}

static void write_lock(const char *text, double age_milliseconds) {
    write_text("processes/" HOST ".lock", text);
    if (age_milliseconds <= 0) {
        return;
    }
    char *path = lock_path();
    double at = runtime_now_milliseconds() - age_milliseconds;
#if defined(_WIN32)
    wchar_t wide[1024];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 1024);
    HANDLE handle = CreateFileW(wide, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING, 0, NULL);
    ULONGLONG ticks = (ULONGLONG)(at * 10000.0) + 116444736000000000ULL;
    FILETIME time = {(DWORD)ticks, (DWORD)(ticks >> 32)};
    CHECK(SetFileTime(handle, NULL, &time, &time) != 0);
    CloseHandle(handle);
#else
    struct timespec times[2];
    times[0].tv_sec = (time_t)(at / 1000.0);
    times[0].tv_nsec = 0;
    times[1] = times[0];
    CHECK(utimensat(AT_FDCWD, path, times, 0) == 0);
#endif
    free(path);
}

typedef struct lock_events {
    volatile long next;
    volatile long first_in;
    volatile long first_out;
    volatile long second_in;
    volatile long entered;
} lock_events;

static lock_events events;

static long next_event(void) {
#if defined(_WIN32)
    return InterlockedIncrement(&events.next);
#else
    return __atomic_add_fetch(&events.next, 1, __ATOMIC_SEQ_CST);
#endif
}

static void hold_lock_for_a_while(void) {
    runtime_holders_guard *guard = take_lock();
    events.first_in = next_event();
    pause_milliseconds(200);
    events.first_out = next_event();
    runtime_unlock_holders(guard);
}

static void enter_lock(void) {
    runtime_holders_guard *guard = take_lock();
    events.entered = 1;
    runtime_unlock_holders(guard);
}

#if defined(_WIN32)
typedef HANDLE test_thread;

static DWORD WINAPI first_locker(LPVOID unused) {
    (void)unused;
    hold_lock_for_a_while();
    return 0;
}

static DWORD WINAPI waiting_locker(LPVOID unused) {
    (void)unused;
    enter_lock();
    return 0;
}

static test_thread start_thread(int first) {
    return CreateThread(NULL, 0, first ? first_locker : waiting_locker, NULL, 0, NULL);
}

static void join_thread(test_thread thread) {
    WaitForSingleObject(thread, 30000);
    CloseHandle(thread);
}
#else
typedef pthread_t test_thread;

static void *first_locker(void *unused) {
    (void)unused;
    hold_lock_for_a_while();
    return NULL;
}

static void *waiting_locker(void *unused) {
    (void)unused;
    enter_lock();
    return NULL;
}

static test_thread start_thread(int first) {
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, first ? first_locker : waiting_locker, NULL) == 0);
    return thread;
}

static void join_thread(test_thread thread) {
    pthread_join(thread, NULL);
}
#endif

static int first_is_in(const void *unused) {
    (void)unused;
    return events.first_in != 0;
}

static int has_entered(const void *unused) {
    (void)unused;
    return events.entered != 0;
}

static void it_runs_one_holder_of_the_lock_at_a_time_and_removes_the_lock_afterwards(void) {
    fresh_state("lock-order");
    memset((void *)&events, 0, sizeof events);
    test_thread first = start_thread(1);
    CHECK(wait_until(first_is_in, NULL, 5000));
    runtime_holders_guard *guard = take_lock();
    events.second_in = next_event();
    runtime_unlock_holders(guard);
    join_thread(first);
    CHECK(events.first_in == 1 && events.first_out == 2 && events.second_in == 3);
    CHECK(!present("processes/" HOST ".lock"));
    finish_case();
}

static void it_takes_a_lock_whose_process_is_dead(void) {
    fresh_state("lock-dead");
    child_process dead = exited_child();
    write_lock(number_text((double)dead.pid), 0);
    double started = runtime_now_milliseconds();
    runtime_unlock_holders(take_lock());
    CHECK(runtime_now_milliseconds() - started < 2000);
    release_child(&dead);
    finish_case();
}

static void it_takes_a_lock_older_than_ten_seconds_even_when_its_process_is_alive(void) {
    fresh_state("lock-old");
    write_lock(number_text(self_pid()), 20000);
    double started = runtime_now_milliseconds();
    runtime_unlock_holders(take_lock());
    CHECK(runtime_now_milliseconds() - started < 2000);
    finish_case();
}

static void it_leaves_a_lock_that_is_no_longer_its_own(void) {
    fresh_state("lock-replaced");
    runtime_holders_guard *guard = take_lock();
    char other[64];
    snprintf(other, sizeof other, "%d another-locker", (int)current_process_id());
    write_lock(other, 0);
    runtime_unlock_holders(guard);
    char *text = read_text("processes/" HOST ".lock");
    check_text(text, other, "replaced lock");
    free(text);
    finish_case();
}

static void it_waits_for_a_fresh_lock_a_live_process_holds(void) {
    fresh_state("lock-wait");
    memset((void *)&events, 0, sizeof events);
    write_lock(number_text(self_pid()), 0);
    test_thread waiting = start_thread(0);
    pause_milliseconds(300);
    CHECK(events.entered == 0);
    char *path = lock_path();
    CHECK(runtime_remove_file_entry(path) == RUNTIME_ENTRY_DONE);
    free(path);
    CHECK(wait_until(has_entered, NULL, 5000));
    join_thread(waiting);
    finish_case();
}

static void it_waits_for_an_empty_lock_until_it_is_ten_seconds_old(void) {
    fresh_state("lock-empty");
    memset((void *)&events, 0, sizeof events);
    write_lock("", 0);
    test_thread waiting = start_thread(0);
    pause_milliseconds(300);
    CHECK(events.entered == 0);
    write_lock("", 20000);
    CHECK(wait_until(has_entered, NULL, 5000));
    join_thread(waiting);
    finish_case();
}

static void it_refuses_a_lock_path_that_is_a_directory_rather_than_waiting(void) {
    fresh_state("lock-directory");
    char *path = lock_path();
    CHECK(spisor_make_directories(path) == 1);
    double started = runtime_now_milliseconds();
    CHECK(runtime_lock_holders(path) == NULL);
    catalog_answer answer = hold(self_pid(), sleep_start());
    CHECK(runtime_now_milliseconds() - started < 2000);
    CHECK(answer.status != 0);
    const char *message = text_at(answer.tree, "message");
    const char *fix = text_at(answer.tree, "fix");
    CHECK(message != NULL && strstr(message, "the holders lock at ") != NULL && strstr(message, HOST ".lock") != NULL);
    CHECK(fix != NULL && strstr(fix, HOST ".lock") != NULL);
    CHECK(!holders_file_present());
    cJSON_Delete(answer.tree);
    free(path);
    finish_case();
}

/* holders.test.ts: isHostRunning, as hold judges the recorded host */

static void expect_kept(double running) {
    catalog_answer answer = hold(self_pid(), sleep_start());
    CHECK(answer.status == 0);
    CHECK(answered_flag(&answer, "started") == 0);
    CHECK(answered_pid(&answer) == running);
    CHECK(cJSON_GetObjectItemCaseSensitive(answer.tree, "outsideJob") == NULL);
    cJSON_Delete(answer.tree);
}

static void it_keeps_a_live_pid_stamped_within_three_of_its_checks(void) {
    fresh_state("running-fresh");
    write_record(number_text(self_pid()), number_text(runtime_now_milliseconds() - 5500), "2000", "[]");
    expect_kept(self_pid());
    finish_case();
}

static void it_starts_a_host_when_a_live_pid_s_stamp_is_older_than_three_checks(void) {
    fresh_state("running-stale");
    write_record(number_text(self_pid()), number_text(runtime_now_milliseconds() - 6500), "2000", "[]");
    double pid = hold_starting(sleep_start());
    CHECK(pid != self_pid() && runtime_process_is_alive(pid));
    finish_case();
}

static void it_starts_a_host_for_a_dead_pid_and_for_a_record_never_stamped(void) {
    fresh_state("running-dead");
    child_process dead = exited_child();
    write_record(number_text((double)dead.pid), number_text(runtime_now_milliseconds()), "200", "[]");
    CHECK(hold_starting(sleep_start()) != (double)dead.pid);
    release_child(&dead);
    finish_case();
    fresh_state("running-never-stamped");
    write_record(number_text(self_pid()), "null", "null", "[]");
    CHECK(hold_starting(sleep_start()) != self_pid());
    finish_case();
}

static void it_judges_a_host_that_has_not_checked_yet_by_the_default_interval(void) {
    fresh_state("running-default");
    write_record(number_text(self_pid()), number_text(runtime_now_milliseconds() - 14000), "null", "[]");
    expect_kept(self_pid());
    finish_case();
}

static void it_tells_this_process_alive_and_an_exited_one_dead(void) {
    child_process dead = exited_child();
    CHECK(runtime_process_is_alive(self_pid()));
    CHECK(!runtime_process_is_alive((double)dead.pid));
    release_child(&dead);
}

/* background.test.ts: hold and release */

static void it_starts_one_process_for_two_holders_and_stamps_it_alive(void) {
    fresh_state("two-holders");
    child_process other = start_sleeper();
    double before = floor(runtime_now_milliseconds());
    double first = hold_starting(sleep_start());
    catalog_answer second = hold((double)other.pid, sleep_start());
    CHECK(second.status == 0 && answered_flag(&second, "started") == 0 && answered_pid(&second) == first);
    cJSON_Delete(second.tree);
    runtime_holders_record record = read_record();
    double expected[2] = {self_pid(), (double)other.pid};
    CHECK(record.pid == first && isnan(record.check_milliseconds) && holders_are(&record, expected, 2));
    CHECK(record.alive_at >= before);
    runtime_free_holders(&record);
    kill_child(&other);
    release_child(&other);
    finish_case();
}

static void it_only_drops_the_holder_on_release_and_never_signals_the_host(void) {
    fresh_state("release-only");
    child_process other = start_sleeper();
    double pid = hold_starting(sleep_start());
    catalog_answer second = hold((double)other.pid, sleep_start());
    cJSON_Delete(second.tree);
    catalog_answer first_release = release(self_pid());
    CHECK(first_release.status == 0);
    cJSON_Delete(first_release.tree);
    runtime_holders_record record = read_record();
    double left[1] = {(double)other.pid};
    CHECK(record.pid == pid && holders_are(&record, left, 1));
    runtime_free_holders(&record);
    catalog_answer last_release = release((double)other.pid);
    const cJSON *holders = cJSON_GetObjectItemCaseSensitive(last_release.tree, "holders");
    CHECK(last_release.status == 0 && cJSON_IsArray(holders) && cJSON_GetArraySize(holders) == 0);
    cJSON_Delete(last_release.tree);
    pause_milliseconds(300);
    CHECK(runtime_process_is_alive(pid));
    record = read_record();
    CHECK(record.pid == pid && record.count == 0);
    runtime_free_holders(&record);
    kill_child(&other);
    release_child(&other);
    finish_case();
}

static void it_starts_a_new_process_when_the_recorded_one_is_dead(void) {
    fresh_state("recorded-dead");
    child_process dead = exited_child();
    write_record(number_text((double)dead.pid), number_text(runtime_now_milliseconds()), "200", "[]");
    double pid = hold_starting(sleep_start());
    CHECK(pid != (double)dead.pid);
    runtime_holders_record record = read_record();
    double expected[1] = {self_pid()};
    CHECK(record.pid == pid && holders_are(&record, expected, 1));
    runtime_free_holders(&record);
    release_child(&dead);
    finish_case();
}

static void it_starts_a_fresh_host_when_the_recorded_pid_is_alive_but_its_stamp_is_stale(void) {
    fresh_state("recorded-stale");
    child_process unrelated = start_sleeper();
    write_record(number_text((double)unrelated.pid), number_text(runtime_now_milliseconds() - 2000), "200", "[]");
    double pid = hold_starting(sleep_start());
    CHECK(pid != (double)unrelated.pid);
    CHECK(runtime_process_is_alive((double)unrelated.pid));
    kill_child(&unrelated);
    release_child(&unrelated);
    finish_case();
}

static void it_keeps_a_host_whose_stamp_is_fresh(void) {
    fresh_state("recorded-fresh");
    child_process running = start_sleeper();
    write_record(number_text((double)running.pid), number_text(runtime_now_milliseconds()), "200", "[]");
    expect_kept((double)running.pid);
    kill_child(&running);
    release_child(&running);
    finish_case();
}

static void it_holds_once_however_often_the_same_holder_asks(void) {
    fresh_state("holds-once");
    hold_starting(sleep_start());
    catalog_answer again = hold(self_pid(), sleep_start());
    double pid = answered_pid(&again);
    cJSON_Delete(again.tree);
    runtime_holders_record record = read_record();
    double expected[1] = {self_pid()};
    CHECK(record.pid == pid && holders_are(&record, expected, 1));
    runtime_free_holders(&record);
    finish_case();
}

static void it_refuses_a_start_it_cannot_run_leaving_nothing_held(void) {
    fresh_state("bad-start");
    cJSON *relative = cJSON_CreateObject();
    cJSON_AddStringToObject(relative, "executable", "background_fixture");
    catalog_answer answer = hold(self_pid(), relative);
    CHECK(answer.status != 0 && text_at(answer.tree, "message") != NULL && text_at(answer.tree, "fix") != NULL);
    cJSON_Delete(answer.tree);
    cJSON *missing = cJSON_CreateObject();
    char *nowhere = joined(state, "no-such-executable");
    cJSON_AddStringToObject(missing, "executable", nowhere);
    free(nowhere);
    answer = hold(self_pid(), missing);
    CHECK(answer.status != 0 && text_at(answer.tree, "message") != NULL && text_at(answer.tree, "fix") != NULL);
    cJSON_Delete(answer.tree);
    CHECK(!holders_file_present());
    finish_case();
}

static int answer_written(const void *path) {
    return spisor_file_exists((const char *)path);
}

static void it_starts_the_host_in_the_environment_and_directory_given(void) {
    fresh_state("environment");
    char *answer_path = joined(state, "environment.json");
    char *directory = joined(state, "work");
    CHECK(spisor_make_directories(directory) == 1);
    const char *arguments[3] = {"environment", answer_path, "SPISOR_FIXTURE_VALUE"};
    cJSON *start = start_of(arguments, 3);
    cJSON *environment = cJSON_AddObjectToObject(start, "environment");
    cJSON_AddStringToObject(environment, "SPISOR_FIXTURE_VALUE", "kept");
    cJSON_AddStringToObject(start, "directory", directory);
    hold_starting(start);
    CHECK(wait_until(answer_written, answer_path, 10000));
    pause_milliseconds(100);
    cJSON *seen = read_json_file(answer_path);
    check_text(text_at(seen, "value"), "kept", "environment value");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(seen, "path")));
    const char *where = text_at(seen, "directory");
    CHECK(where != NULL && strstr(where, "work") != NULL);
    cJSON_Delete(seen);
    free(directory);
    free(answer_path);
    finish_case();
}

/* self-exit.test.ts: prune */

static int prune_exits(double self, double now) {
    catalog_answer answer = prune(self, now);
    CHECK(answer.status == 0);
    int exits = answered_flag(&answer, "exit");
    CHECK(exits != -1);
    cJSON_Delete(answer.tree);
    return exits;
}

static void it_keeps_running_while_a_holder_lives_and_exits_once_the_last_dies(void) {
    fresh_state("prune-last");
    child_process holder = start_sleeper();
    char holders[64];
    snprintf(holders, sizeof holders, "[%ld]", holder.pid);
    write_record(number_text(self_pid()), "null", "null", holders);
    CHECK(prune_exits(self_pid(), runtime_now_milliseconds()) == 0);
    CHECK(prune_exits(self_pid(), runtime_now_milliseconds()) == 0);
    kill_child(&holder);
    CHECK(prune_exits(self_pid(), runtime_now_milliseconds()) == 1);
    CHECK(!holders_file_present());
    release_child(&holder);
    finish_case();
}

static void it_prunes_a_dead_holder_and_keeps_the_live_ones(void) {
    fresh_state("prune-dead");
    child_process living = start_sleeper();
    child_process dying = start_sleeper();
    char holders[64];
    snprintf(holders, sizeof holders, "[%ld,%ld]", living.pid, dying.pid);
    write_record(number_text(self_pid()), "null", "null", holders);
    kill_child(&dying);
    CHECK(prune_exits(self_pid(), 123456) == 0);
    runtime_holders_record record = read_record();
    double expected[1] = {(double)living.pid};
    CHECK(record.pid == self_pid() && record.check_milliseconds == INTERVAL && record.alive_at == 123456 &&
          holders_are(&record, expected, 1));
    runtime_free_holders(&record);
    kill_child(&living);
    release_child(&living);
    release_child(&dying);
    finish_case();
}

static void it_exits_without_touching_the_record_when_another_process_is_the_host(void) {
    fresh_state("prune-replaced");
    child_process holder = start_sleeper();
    char holders[64];
    snprintf(holders, sizeof holders, "[%ld]", holder.pid);
    write_record(number_text((double)holder.pid), "5", INTERVAL_TEXT, holders);
    char *before = read_text("processes/" HOST ".json");
    CHECK(prune_exits(self_pid(), runtime_now_milliseconds()) == 1);
    char *after = read_text("processes/" HOST ".json");
    check_text(after, before, "untouched record");
    free(before);
    free(after);
    kill_child(&holder);
    release_child(&holder);
    finish_case();
}

static void it_exits_at_once_when_nothing_holds_it(void) {
    fresh_state("prune-nothing");
    CHECK(prune_exits(self_pid(), runtime_now_milliseconds()) == 1);
    CHECK(!holders_file_present());
    finish_case();
}

/* launch.test.ts and background-run.test.ts: real holders and a real background host */

typedef struct stamped_by {
    double pid;
    size_t holders;
} stamped_by;

static int is_stamped_by(const void *context) {
    const stamped_by *expected = context;
    char *path = joined(state, "processes/" HOST ".json");
    runtime_holders_record record = runtime_no_holders();
    int read = runtime_read_holders(path, &record);
    free(path);
    int stamped = read && record.pid == expected->pid && record.check_milliseconds == INTERVAL &&
                  record.count == expected->holders;
    runtime_free_holders(&record);
    return stamped;
}

static void expect_stamped(double pid, size_t holders) {
    stamped_by expected = {pid, holders};
    CHECK(wait_until(is_stamped_by, &expected, 5000));
}

static void expect_gone_within_two_intervals(double pid) {
    CHECK(wait_until(is_gone, &pid, INTERVAL * 2 + ALLOWANCE));
}

typedef struct launcher {
    child_process process;
    char *answer;
} launcher;

static launcher start_launcher(const char *name, int suspended, void *thread) {
    launcher started;
    started.answer = joined(state, name);
    const char *arguments[9] = {"hold", state, HOST, started.answer, FIXTURE, "background", state, HOST, INTERVAL_TEXT};
    started.process = start_child(FIXTURE, arguments, 9, suspended, thread);
    return started;
}

/** What the launcher's hold answered, once it has. */
static cJSON *launcher_answer(const launcher *started) {
    if (!wait_until(answer_written, started->answer, 10000)) {
        return NULL;
    }
    return read_json_file(started->answer);
}

static double launched_host(const launcher *started, int expect_started) {
    cJSON *answer = launcher_answer(started);
    const cJSON *pid = cJSON_GetObjectItemCaseSensitive(answer, "pid");
    CHECK(cJSON_IsNumber(pid));
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answer, "started")) == expect_started);
    double host = cJSON_IsNumber(pid) ? pid->valuedouble : NAN;
    remember_detached(host);
    cJSON_Delete(answer);
    return host;
}

static void end_launcher(launcher *started) {
    kill_child(&started->process);
    release_child(&started->process);
    free(started->answer);
}

static void hold_for(double holder) {
    catalog_answer answer = hold(holder, background_start());
    CHECK(answer.status == 0 && answered_flag(&answer, "started") == 0);
    cJSON_Delete(answer.tree);
}

static void one_holder_starts_one_fixture_and_its_release_stops_it(void) {
    fresh_state("fixture-release");
    double host = hold_starting(background_start());
    expect_stamped(host, 1);
    CHECK(runtime_process_is_alive(host));
    catalog_answer released = release(self_pid());
    CHECK(released.status == 0);
    cJSON_Delete(released.tree);
    expect_gone_within_two_intervals(host);
    CHECK(!holders_file_present());
    finish_case();
}

static void two_holders_share_one_process(void) {
    fresh_state("fixture-shared");
    launcher first = start_launcher("first.json", 0, NULL);
    double host = launched_host(&first, 1);
    expect_stamped(host, 1);
    launcher second = start_launcher("second.json", 0, NULL);
    CHECK(launched_host(&second, 0) == host);
    expect_stamped(host, 2);
    kill_child(&first.process);
    pause_milliseconds((long)(INTERVAL * 3));
    CHECK(runtime_process_is_alive(host));
    runtime_holders_record record = read_record();
    double left[1] = {(double)second.process.pid};
    CHECK(record.pid == host && holders_are(&record, left, 1));
    runtime_free_holders(&record);
    kill_child(&second.process);
    expect_gone_within_two_intervals(host);
    CHECK(!holders_file_present());
    end_launcher(&first);
    end_launcher(&second);
    finish_case();
}

static void a_holder_killed_hard_leaves_a_fixture_that_exits_within_two_intervals(void) {
    fresh_state("fixture-killed");
    launcher started = start_launcher("answer.json", 0, NULL);
    double host = launched_host(&started, 1);
    expect_stamped(host, 1);
    kill_child(&started.process);
    expect_gone_within_two_intervals(host);
    CHECK(!holders_file_present());
    end_launcher(&started);
    finish_case();
}

static void it_keeps_the_host_for_an_application_whose_launcher_is_killed_hard(void) {
    fresh_state("fixture-application");
    child_process application = start_sleeper();
    launcher started = start_launcher("answer.json", 0, NULL);
    double host = launched_host(&started, 1);
    hold_for((double)application.pid);
    expect_stamped(host, 2);
    kill_child(&started.process);
    pause_milliseconds((long)(INTERVAL * 3));
    CHECK(runtime_process_is_alive(host));
    runtime_holders_record record = read_record();
    double left[1] = {(double)application.pid};
    CHECK(record.pid == host && holders_are(&record, left, 1));
    runtime_free_holders(&record);
    kill_child(&application);
    expect_gone_within_two_intervals(host);
    release_child(&application);
    end_launcher(&started);
    finish_case();
}

#if defined(_WIN32)

static HANDLE job_with(DWORD limits) {
    HANDLE job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION information;
    memset(&information, 0, sizeof information);
    information.BasicLimitInformation.LimitFlags = limits;
    CHECK(job != NULL);
    CHECK(SetInformationJobObject(job, JobObjectExtendedLimitInformation, &information, sizeof information) != 0);
    return job;
}

static launcher start_launcher_in(HANDLE job) {
    HANDLE thread = NULL;
    launcher started = start_launcher("answer.json", 1, &thread);
    CHECK(AssignProcessToJobObject(job, started.process.handle) != 0);
    CHECK(ResumeThread(thread) != (DWORD)-1);
    CloseHandle(thread);
    return started;
}

/** Whether this test's own job, if it runs in one, lets its children break away. */
static int this_job_allows_breakaway(void) {
    BOOL in_job = FALSE;
    if (!IsProcessInJob(GetCurrentProcess(), NULL, &in_job) || !in_job) {
        return 1;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION information;
    if (!QueryInformationJobObject(NULL, JobObjectExtendedLimitInformation, &information, sizeof information, NULL)) {
        return 0;
    }
    return (information.BasicLimitInformation.LimitFlags &
            (JOB_OBJECT_LIMIT_BREAKAWAY_OK | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK)) != 0;
}

static int launcher_outside_job(const launcher *started) {
    cJSON *answer = launcher_answer(started);
    int outside = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answer, "outsideJob"));
    CHECK(cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(answer, "outsideJob")));
    cJSON_Delete(answer);
    return outside;
}

static void it_breaks_away_from_a_launcher_s_job_that_allows_it(void) {
    fresh_state("job-breakaway");
    child_process application = start_sleeper();
    HANDLE job = job_with(JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK);
    launcher started = start_launcher_in(job);
    double host = launched_host(&started, 1);
    int outside = launcher_outside_job(&started);
    if (!this_job_allows_breakaway()) {
        printf("processes_test: the test runs in a job that forbids breakaway, so only the fallback is seen\n");
        CHECK(!outside);
    } else {
        CHECK(outside);
        hold_for((double)application.pid);
        expect_stamped(host, 2);
        CloseHandle(job);
        job = NULL;
        CHECK(WaitForSingleObject(started.process.handle, 10000) == WAIT_OBJECT_0);
        pause_milliseconds((long)(INTERVAL * 3));
        CHECK(runtime_process_is_alive(host));
        kill_child(&application);
        expect_gone_within_two_intervals(host);
    }
    if (job != NULL) {
        CloseHandle(job);
    }
    kill_child(&application);
    release_child(&application);
    end_launcher(&started);
    finish_case();
}

static void it_starts_inside_a_job_that_forbids_breaking_away_and_says_so(void) {
    fresh_state("job-forbidden");
    HANDLE job = job_with(JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE);
    launcher started = start_launcher_in(job);
    double host = launched_host(&started, 1);
    CHECK(!launcher_outside_job(&started));
    CloseHandle(job);
    CHECK(wait_until(is_gone, &host, ALLOWANCE));
    end_launcher(&started);
    finish_case();
}

#endif

/* run-records.test.ts: a start's run record through processes */

static catalog_answer record_run(const char *host_id, const char *run, const char *record) {
    cJSON *argument = host_argument(host_id);
    cJSON_AddItemToObject(argument, "run", parsed(run));
    cJSON_AddNumberToObject(argument, "checkMilliseconds", 1000);
    if (record != NULL) {
        cJSON_AddStringToObject(argument, "record", record);
    }
    return call_catalog(runtime_processes_record_run, argument);
}

static char *recorded_path(const char *host_id, const char *run) {
    catalog_answer answer = record_run(host_id, run, NULL);
    CHECK(answer.status == 0);
    const char *path = text_at(answer.tree, "record");
    char *copy = path == NULL ? NULL : malloc(strlen(path) + 1);
    if (copy != NULL) {
        memcpy(copy, path, strlen(path) + 1);
    }
    cJSON_Delete(answer.tree);
    return copy;
}

static cJSON *live_runs(void) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "state", state);
    catalog_answer answer = call_catalog(runtime_processes_live_runs, argument);
    CHECK(answer.status == 0);
    return answer.tree;
}

static void expect_runs(const char *expected) {
    cJSON *answer = live_runs();
    char *text = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(answer, "runs"));
    check_text(text, expected, "live runs");
    cJSON_free(text);
    cJSON_Delete(answer);
}

static double recorded_alive_at(const char *record) {
    cJSON *recorded = read_json_file(record);
    const cJSON *alive_at = cJSON_GetObjectItemCaseSensitive(recorded, "aliveAt");
    double at = cJSON_IsNumber(alive_at) ? alive_at->valuedouble : NAN;
    CHECK(cJSON_GetObjectItemCaseSensitive(recorded, "pid")->valuedouble == self_pid());
    CHECK(cJSON_GetObjectItemCaseSensitive(recorded, "checkMilliseconds")->valuedouble == 1000);
    cJSON_Delete(recorded);
    return at;
}

static void it_records_a_run_and_stamps_the_same_record_again(void) {
    fresh_state("run-stamp");
    char *record = recorded_path("fixture-loader", "{\"a\":\"1.0.0\"}");
    CHECK(record != NULL && spisor_file_exists(record));
    double first = recorded_alive_at(record);
    pause_milliseconds(30);
    catalog_answer again = record_run("fixture-loader", "{\"a\":\"1.0.0\"}", record);
    CHECK(again.status == 0);
    check_text(text_at(again.tree, "record"), record, "stamped record");
    cJSON_Delete(again.tree);
    CHECK(recorded_alive_at(record) > first);
    expect_runs("[{\"a\":\"1.0.0\"}]");
    free(record);
    finish_case();
}

static void it_refuses_to_stamp_a_record_that_is_no_run_record_of_the_host(void) {
    fresh_state("run-outside");
    char *record = recorded_path("fixture-loader", "{\"a\":\"1.0.0\"}");
    char *elsewhere = joined(state, "elsewhere.json");
    const char *refused[3] = {elsewhere, record, "relative.json"};
    const char *hosts[3] = {"fixture-loader", "another-loader", "fixture-loader"};
    for (size_t index = 0; index < 3; index++) {
        catalog_answer answer = record_run(hosts[index], "{\"a\":\"1.0.0\"}", refused[index]);
        CHECK(answer.status != 0 && text_at(answer.tree, "message") != NULL && text_at(answer.tree, "fix") != NULL);
        cJSON_Delete(answer.tree);
    }
    CHECK(!spisor_file_exists(elsewhere));
    CHECK(!present("bundles/runs/another-loader"));
    free(elsewhere);
    free(record);
    finish_case();
}

static void write_run(const char *name, double pid, double alive_at, const char *run) {
    char rest[256];
    snprintf(rest, sizeof rest, "bundles/runs/fixture-loader/%s", name);
    char text[256];
    snprintf(text, sizeof text, "{\"pid\":%.0f,\"aliveAt\":%.0f,\"checkMilliseconds\":5000,\"run\":%s}\n", pid, alive_at,
             run);
    write_text(rest, text);
}

static void it_answers_live_runs_and_prunes_one_whose_process_is_gone(void) {
    fresh_state("runs-dead");
    child_process dead = exited_child();
    write_run("dead.json", (double)dead.pid, runtime_now_milliseconds(), "{\"a\":\"1.0.0\"}");
    write_run("live.json", self_pid(), runtime_now_milliseconds(), "{\"a\":\"2.0.0\"}");
    expect_runs("[{\"a\":\"2.0.0\"}]");
    CHECK(!present("bundles/runs/fixture-loader/dead.json"));
    CHECK(present("bundles/runs/fixture-loader/live.json"));
    release_child(&dead);
    finish_case();
}

static void it_prunes_a_run_whose_process_stopped_stamping(void) {
    fresh_state("runs-stale");
    write_run("stale.json", self_pid(), runtime_now_milliseconds() - 60000, "{\"a\":\"1.0.0\"}");
    write_text("bundles/runs/fixture-loader/unreadable.json", "{");
    expect_runs("[]");
    CHECK(!present("bundles/runs/fixture-loader/stale.json"));
    CHECK(!present("bundles/runs/fixture-loader/unreadable.json"));
    finish_case();
}

static catalog_answer remove_runs(const char *record) {
    cJSON *argument = host_argument("fixture-loader");
    if (record != NULL) {
        cJSON_AddStringToObject(argument, "record", record);
    }
    return call_catalog(runtime_processes_remove_runs, argument);
}

static void it_removes_one_run_record_or_every_run_of_the_host(void) {
    fresh_state("runs-remove");
    char *first = recorded_path("fixture-loader", "{\"a\":\"1.0.0\"}");
    char *second = recorded_path("fixture-loader", "{\"a\":\"2.0.0\"}");
    catalog_answer one = remove_runs(first);
    CHECK(one.status == 0);
    cJSON_Delete(one.tree);
    CHECK(!spisor_file_exists(first) && spisor_file_exists(second));
    catalog_answer every = remove_runs(NULL);
    CHECK(every.status == 0);
    cJSON_Delete(every.tree);
    CHECK(!present("bundles/runs/fixture-loader"));
    expect_runs("[]");
    free(first);
    free(second);
    finish_case();
}

int main(void) {
    it_reads_back_what_was_written_and_reads_as_empty_when_there_is_none();
    it_removes_the_file_once_it_records_neither_a_process_nor_a_holder();
    it_reads_a_file_that_is_not_a_holders_record_as_empty();
    it_refuses_a_host_id_that_would_place_the_file_outside_the_processes_folder();
    it_runs_one_holder_of_the_lock_at_a_time_and_removes_the_lock_afterwards();
    it_takes_a_lock_whose_process_is_dead();
    it_takes_a_lock_older_than_ten_seconds_even_when_its_process_is_alive();
    it_leaves_a_lock_that_is_no_longer_its_own();
    it_waits_for_a_fresh_lock_a_live_process_holds();
    it_waits_for_an_empty_lock_until_it_is_ten_seconds_old();
    it_refuses_a_lock_path_that_is_a_directory_rather_than_waiting();
    it_keeps_a_live_pid_stamped_within_three_of_its_checks();
    it_starts_a_host_when_a_live_pid_s_stamp_is_older_than_three_checks();
    it_starts_a_host_for_a_dead_pid_and_for_a_record_never_stamped();
    it_judges_a_host_that_has_not_checked_yet_by_the_default_interval();
    it_tells_this_process_alive_and_an_exited_one_dead();
    it_starts_one_process_for_two_holders_and_stamps_it_alive();
    it_only_drops_the_holder_on_release_and_never_signals_the_host();
    it_starts_a_new_process_when_the_recorded_one_is_dead();
    it_starts_a_fresh_host_when_the_recorded_pid_is_alive_but_its_stamp_is_stale();
    it_keeps_a_host_whose_stamp_is_fresh();
    it_holds_once_however_often_the_same_holder_asks();
    it_refuses_a_start_it_cannot_run_leaving_nothing_held();
    it_starts_the_host_in_the_environment_and_directory_given();
    it_keeps_running_while_a_holder_lives_and_exits_once_the_last_dies();
    it_prunes_a_dead_holder_and_keeps_the_live_ones();
    it_exits_without_touching_the_record_when_another_process_is_the_host();
    it_exits_at_once_when_nothing_holds_it();
    one_holder_starts_one_fixture_and_its_release_stops_it();
    two_holders_share_one_process();
    a_holder_killed_hard_leaves_a_fixture_that_exits_within_two_intervals();
    it_keeps_the_host_for_an_application_whose_launcher_is_killed_hard();
#if defined(_WIN32)
    it_breaks_away_from_a_launcher_s_job_that_allows_it();
    it_starts_inside_a_job_that_forbids_breaking_away_and_says_so();
#endif
    it_records_a_run_and_stamps_the_same_record_again();
    it_refuses_to_stamp_a_record_that_is_no_run_record_of_the_host();
    it_answers_live_runs_and_prunes_one_whose_process_is_gone();
    it_prunes_a_run_whose_process_stopped_stamping();
    it_removes_one_run_record_or_every_run_of_the_host();
    return check_summary("processes_test");
}
