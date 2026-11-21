#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <string.h>

#include "check.h"
#include "runtime/processes.h"
#include "scope_fixture.h"
#include "spisor/json_allocation.h"
#include "thread.h"

/**
 * @file
 * A scope's operations called from two threads at once, as a host program calls `tick` on a worker while a plugin
 * reads its config on the main thread (rev 225). The blocking operation is processes' await_ready for a host that never
 * becomes ready: of what a host reaches without the network it waits longest, polling the state place until its
 * timeout, as `tick`'s prune and update check would.
 */

#define AWAIT_MILLISECONDS 1500
#define SECRETS_PER_THREAD 40

static scope_place place;

typedef struct blocking_wait {
    const char *state;
    spisor_status status;
    volatile long finished;
} blocking_wait;

static void await_a_host_never_ready(void *argument) {
    blocking_wait *blocked = argument;
    const runtime_processes_definition *processes = runtime_processes_definition_of();
    runtime_processes_await_argument await = {sizeof await, NULL, "never-ready", 1, AWAIT_MILLISECONDS};
    runtime_processes_await_answer answer;
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    memset(&answer, 0, sizeof answer);
    answer.size = (uint32_t)sizeof answer;
    await.state = blocked->state;
    blocked->status = processes->await_ready(processes->context, &await, &answer, &refusal);
    processes->release_await_answer(processes->context, &answer);
    processes->release_refusal(processes->context, &refusal);
    runtime_atomic_increment(&blocked->finished);
}

static int has_finished(blocking_wait *blocked) {
#if defined(_WIN32)
    return InterlockedCompareExchange(&blocked->finished, 0, 0) != 0;
#else
    return __atomic_load_n(&blocked->finished, __ATOMIC_ACQUIRE) != 0;
#endif
}

static int reads_theme(const weivel_standard_config *config) {
    weivel_standard_setting_answer answer = WEIVEL_INIT(weivel_standard_setting_answer);
    int read = config->get(config->context, "theme", &answer, NULL) == WEIVEL_STATUS_OK &&
               answer.value.kind == WEIVEL_STANDARD_SETTING_TEXT && strcmp(answer.value.text, "light") == 0;
    config->release_answer(config->context, &answer);
    return read;
}

static weivel_status open_viewer(weivel_standard_scope *scope) {
    const weivel_standard_scopes *scopes = scopes_definition();
    return scopes->open(scopes->context, place.manifest, scope, NULL);
}

static void config_reads_while_another_thread_blocks(void) {
    weivel_standard_scope scope = WEIVEL_INIT(weivel_standard_scope);
    blocking_wait blocked = {NULL, SPISOR_OK, 0};
    runtime_thread waiter;
    char *state = joined_path(place.base, "state");
    long reads = 0;
    long failed_reads = 0;
    blocked.state = state;
    CHECK(open_viewer(&scope) == WEIVEL_STATUS_OK);
    CHECK(scope.config != NULL && runtime_thread_start(&waiter, await_a_host_never_ready, &blocked));
    while (scope.config != NULL && !has_finished(&blocked)) {
        if (reads_theme(scope.config)) {
            reads++;
        } else {
            failed_reads++;
        }
    }
    runtime_thread_join(&waiter);
    printf("%ld reads of config.get while await_ready blocked, %ld failed\n", reads, failed_reads);
    CHECK(reads > 0 && failed_reads == 0 && blocked.status == SPISOR_REFUSED);
    scopes_definition()->release_scope(NULL, &scope);
    spisor_memory_release(state);
}

typedef struct secret_writer {
    const weivel_standard_secrets *secrets;
    const char *prefix;
    long refused;
} secret_writer;

static void name_of(char *name, size_t size, const char *prefix, int index) {
    snprintf(name, size, "%s-%d", prefix, index);
}

static void set_many_secrets(void *argument) {
    secret_writer *writer = argument;
    char name[32];
    for (int index = 0; index < SECRETS_PER_THREAD; index++) {
        name_of(name, sizeof name, writer->prefix, index);
        if (writer->secrets->set(writer->secrets->context, name, (const uint8_t *)name, strlen(name), NULL) !=
            WEIVEL_STATUS_OK) {
            writer->refused++;
        }
    }
}

static long missing_secrets(const weivel_standard_secrets *secrets, const char *prefix) {
    char name[32];
    long missing = 0;
    for (int index = 0; index < SECRETS_PER_THREAD; index++) {
        weivel_standard_secret secret = WEIVEL_INIT(weivel_standard_secret);
        name_of(name, sizeof name, prefix, index);
        if (secrets->get(secrets->context, name, &secret, NULL) != WEIVEL_STATUS_OK || secret.length != strlen(name)) {
            missing++;
        }
        secrets->release_secret(secrets->context, &secret);
    }
    return missing;
}

/** Two threads setting secrets at once in one secrets file, where each set rewrites the whole file. */
static void secrets_set_from_two_threads_are_all_kept(void) {
    weivel_standard_scope scope = WEIVEL_INIT(weivel_standard_scope);
    secret_writer first = {NULL, "first", 0};
    secret_writer second = {NULL, "second", 0};
    runtime_thread first_thread;
    runtime_thread second_thread;
    long missing;
    CHECK(open_viewer(&scope) == WEIVEL_STATUS_OK);
    if (scope.secrets == NULL) {
        return;
    }
    first.secrets = second.secrets = scope.secrets;
    CHECK(runtime_thread_start(&first_thread, set_many_secrets, &first));
    CHECK(runtime_thread_start(&second_thread, set_many_secrets, &second));
    runtime_thread_join(&first_thread);
    runtime_thread_join(&second_thread);
    missing = missing_secrets(scope.secrets, "first") + missing_secrets(scope.secrets, "second");
    printf("%d secrets set from two threads: %ld refused, %ld missing\n", 2 * SECRETS_PER_THREAD,
           first.refused + second.refused, missing);
    CHECK(first.refused == 0 && second.refused == 0 && missing == 0);
    scopes_definition()->release_scope(NULL, &scope);
}

int main(void) {
    spisor_json_use_library_allocator();
    start_scope_place(&place, "scope-threads", "fixture");
    config_reads_while_another_thread_blocks();
    secrets_set_from_two_threads_are_all_kept();
    runtime_scopes_end();
    scope_place_free(&place);
    return check_summary("scope_threads_test");
}
