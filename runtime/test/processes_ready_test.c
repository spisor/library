#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog_call.h"
#include "check.h"
#include "directory_tree.h"
#include "processes_ready.h"
#include "scratch_directory.h"
#include "spisor/file.h"

#define HOST "session-fixture"

static char state[1024];

static void fresh_state(const char *name) {
    snprintf(state, sizeof state, "%s/processes-ready-%s", RUNTIME_SCRATCH_PATH, name);
    start_empty_directory(state);
}

static cJSON *call_with(const char *tail) {
    char text[1024];
    snprintf(text, sizeof text, "{\"state\":\"%s\",\"hostId\":\"" HOST "\",%s}", state, tail);
    for (char *at = text; *at != 0; at++) {
        *at = *at == '\\' ? '/' : *at;
    }
    return parsed(text);
}

static catalog_answer publish(const char *tail) {
    return call_catalog(runtime_processes_ready, call_with(tail));
}

static catalog_answer await_ready(const char *tail) {
    return call_catalog(runtime_processes_await_ready, call_with(tail));
}

static void it_writes_the_record_with_the_environment_it_publishes(void) {
    fresh_state("write");
    catalog_answer answer = publish("\"pid\":7,\"environment\":{\"SPISOR_TEST\":\"1\"}");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(answer.tree);
    char path[1100];
    snprintf(path, sizeof path, "%s/processes/" HOST ".ready.json", state);
    cJSON *record = read_json_file(path);
    CHECK(value_at(record, "pid")->valuedouble == 7);
    check_text(text_at(value_at(record, "environment"), "SPISOR_TEST"), "1", "published variable");
    cJSON_Delete(record);
    runtime_remove_tree(state);
}

static void it_answers_the_environment_published_for_the_pid_awaited(void) {
    fresh_state("answer");
    cJSON_Delete(publish("\"pid\":7,\"environment\":{\"A\":\"b\"}").tree);
    catalog_answer answer = await_ready("\"pid\":7,\"timeoutMilliseconds\":2000");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    check_text(text_at(value_at(answer.tree, "environment"), "A"), "b", "environment answered");
    cJSON_Delete(answer.tree);
    runtime_remove_tree(state);
}

static void it_answers_an_empty_environment_for_a_host_with_nothing_to_publish(void) {
    fresh_state("empty");
    cJSON_Delete(publish("\"pid\":7").tree);
    catalog_answer answer = await_ready("\"pid\":7,\"timeoutMilliseconds\":2000");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    CHECK(cJSON_IsObject(value_at(answer.tree, "environment")) && value_at(answer.tree, "environment")->child == NULL);
    cJSON_Delete(answer.tree);
    runtime_remove_tree(state);
}

static void it_ignores_a_record_of_another_pid_and_times_out_naming_the_host(void) {
    fresh_state("stale");
    cJSON_Delete(publish("\"pid\":7,\"environment\":{\"A\":\"b\"}").tree);
    catalog_answer answer = await_ready("\"pid\":8,\"timeoutMilliseconds\":300");
    expect_refusal(answer, "the session host of " HOST " did not become ready within 0.3 seconds", "stale record");
    cJSON_Delete(answer.tree);
    runtime_remove_tree(state);
}

static void it_times_out_when_nothing_was_published(void) {
    fresh_state("nothing");
    catalog_answer answer = await_ready("\"pid\":8,\"timeoutMilliseconds\":1000");
    expect_refusal(answer, "did not become ready within 1 seconds", "no record");
    cJSON_Delete(answer.tree);
    runtime_remove_tree(state);
}

static void it_answers_a_published_refusal_as_it_is(void) {
    fresh_state("refused");
    cJSON_Delete(publish("\"pid\":7,\"refused\":\"the proxy could not bind\",\"fix\":\"free the port\"").tree);
    catalog_answer answer = await_ready("\"pid\":7,\"timeoutMilliseconds\":2000");
    expect_refusal(answer, "the proxy could not bind", "refusal message");
    check_text(text_at(answer.tree, "fix"), "free the port", "refusal fix");
    cJSON_Delete(answer.tree);
    runtime_remove_tree(state);
}

static void it_refuses_a_record_that_is_no_usable_publication(void) {
    fresh_state("invalid");
    catalog_answer numeric = publish("\"pid\":7,\"environment\":{\"A\":1}");
    expect_refusal(numeric, "environment", "non-string value");
    cJSON_Delete(numeric.tree);
    catalog_answer unfixed = publish("\"pid\":7,\"refused\":\"no\"");
    expect_refusal(unfixed, "fix", "refusal without a fix");
    cJSON_Delete(unfixed.tree);
    catalog_answer both = publish("\"pid\":7,\"environment\":{},\"refused\":\"no\",\"fix\":\"yes\"");
    expect_refusal(both, "environment", "environment and refusal together");
    cJSON_Delete(both.tree);
    runtime_remove_tree(state);
}

static void it_replaces_the_record_of_an_earlier_pid(void) {
    fresh_state("replace");
    cJSON_Delete(publish("\"pid\":7,\"environment\":{\"A\":\"old\"}").tree);
    cJSON_Delete(publish("\"pid\":9,\"environment\":{\"A\":\"new\"}").tree);
    catalog_answer old = await_ready("\"pid\":7,\"timeoutMilliseconds\":300");
    expect_refusal(old, "did not become ready", "earlier pid");
    cJSON_Delete(old.tree);
    catalog_answer current = await_ready("\"pid\":9,\"timeoutMilliseconds\":2000");
    check_text(text_at(value_at(current.tree, "environment"), "A"), "new", "later pid");
    cJSON_Delete(current.tree);
    runtime_remove_tree(state);
}

int main(void) {
    it_writes_the_record_with_the_environment_it_publishes();
    it_answers_the_environment_published_for_the_pid_awaited();
    it_answers_an_empty_environment_for_a_host_with_nothing_to_publish();
    it_ignores_a_record_of_another_pid_and_times_out_naming_the_host();
    it_times_out_when_nothing_was_published();
    it_answers_a_published_refusal_as_it_is();
    it_refuses_a_record_that_is_no_usable_publication();
    it_replaces_the_record_of_an_earlier_pid();
    return check_summary("processes_ready_test");
}
