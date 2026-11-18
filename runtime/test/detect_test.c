#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "spisor/allocation.h"
#include "definition_fixture.h"
#include "on_path.h"
#include "runtime/detect.h"
#include "service_scratch.h"

typedef struct path_scratch {
    char base[1024];
    char *first;
    char *second;
} path_scratch;

static void fresh(path_scratch *place, const char *label) {
    scratch_in(place->base, sizeof place->base, "detect", label);
    place->first = joined_path(place->base, "first");
    place->second = joined_path(place->base, "second");
    CHECK(spisor_make_directories(place->first) == 1 && spisor_make_directories(place->second) == 1);
}

static void discard(path_scratch *place) {
    spisor_memory_release(place->first);
    spisor_memory_release(place->second);
}

static cJSON *environment_of(const char *path_name, const char *search, const char *extensions) {
    cJSON *environment = cJSON_CreateObject();
    cJSON_AddStringToObject(environment, path_name, search);
    if (extensions != NULL) {
        cJSON_AddStringToObject(environment, "PATHEXT", extensions);
    }
    return environment;
}

static char *search_of(const path_scratch *place, char separator) {
    size_t size = strlen(place->first) + strlen(place->second) + 2;
    char *search = spisor_memory_allocate(size);
    snprintf(search, size, "%s%c%s", place->first, separator, place->second);
    return search;
}

/** What runtime_find_on_path answers for binary, the environment freed. */
static char *found(int windows, const char *binary, cJSON *environment) {
    char *answer = NULL;
    runtime_on_path_status status = runtime_find_on_path(windows, binary, environment, &answer);
    CHECK((status == RUNTIME_ON_PATH_FOUND) == (answer != NULL));
    CHECK(status != RUNTIME_ON_PATH_OUT_OF_MEMORY);
    cJSON_Delete(environment);
    return answer;
}

static void check_found(char *answer, const char *expected, const char *label) {
    check_text(answer, expected, label);
    spisor_memory_release(answer);
}

static void check_none(char *answer, const char *label) {
    if (answer != NULL) {
        fprintf(stderr, "%s: expected nothing, got %s\n", label, answer);
        check_failures++;
    }
    spisor_memory_release(answer);
}

#if !defined(_WIN32)
static void posix_finds_an_executable_file(void) {
    path_scratch place;
    fresh(&place, "executable");
    char *binary = touch_file(place.second, "claude", "", 0755);
    char *search = search_of(&place, ':');
    check_found(found(0, "claude", environment_of("PATH", search, NULL)), binary, "an executable file");
    spisor_memory_release(search);
    spisor_memory_release(binary);
    discard(&place);
}

static void posix_skips_a_same_named_file_without_an_execute_bit(void) {
    path_scratch place;
    fresh(&place, "skips");
    spisor_memory_release(touch_file(place.first, "claude", "", 0644));
    char *executable = touch_file(place.second, "claude", "", 0755);
    char *search = search_of(&place, ':');
    check_found(found(0, "claude", environment_of("PATH", search, NULL)), executable, "the executable one");
    spisor_memory_release(search);
    spisor_memory_release(executable);
    discard(&place);
}

static void posix_answers_nothing_when_only_a_non_executable_file_matches(void) {
    path_scratch place;
    fresh(&place, "plain");
    spisor_memory_release(touch_file(place.first, "claude", "", 0644));
    char *search = search_of(&place, ':');
    check_none(found(0, "claude", environment_of("PATH", search, NULL)), "a non-executable file");
    spisor_memory_release(search);
    discard(&place);
}
#endif

static void posix_does_not_treat_a_directory_as_an_executable(void) {
    path_scratch place;
    fresh(&place, "directory");
    make_directory_in(place.first, "claude");
    char *search = search_of(&place, ':');
    check_none(found(0, "claude", environment_of("PATH", search, NULL)), "a directory");
    spisor_memory_release(search);
    discard(&place);
}

static void posix_ignores_a_path_entry_that_does_not_exist(void) {
    path_scratch place;
    fresh(&place, "missing");
    char *missing = joined_path(place.base, "missing");
    check_none(found(0, "claude", environment_of("PATH", missing, NULL)), "a missing entry");
    spisor_memory_release(missing);
    discard(&place);
}

static void windows_finds_claude_cmd_through_pathext(void) {
    path_scratch place;
    fresh(&place, "pathext");
    char *binary = touch_file(place.first, "claude.cmd", "", 0644);
    char *search = search_of(&place, ';');
    check_found(found(1, "claude", environment_of("Path", search, ".EXE;.CMD")), binary, "claude.cmd");
    spisor_memory_release(search);
    spisor_memory_release(binary);
    discard(&place);
}

static void windows_matches_the_name_and_the_extension_ignoring_case(void) {
    path_scratch place;
    fresh(&place, "case");
    char *binary = touch_file(place.first, "CLAUDE.Exe", "", 0644);
    char *search = search_of(&place, ';');
    check_found(found(1, "claude", environment_of("PATH", search, ".exe")), binary, "CLAUDE.Exe");
    spisor_memory_release(search);
    spisor_memory_release(binary);
    discard(&place);
}

static void windows_defaults_pathext_when_it_is_unset(void) {
    path_scratch place;
    fresh(&place, "default");
    char *binary = touch_file(place.second, "claude.bat", "", 0644);
    char *search = search_of(&place, ';');
    check_found(found(1, "claude", environment_of("PATH", search, NULL)), binary, "claude.bat");
    spisor_memory_release(search);
    spisor_memory_release(binary);
    discard(&place);
}

static void windows_does_not_take_an_extension_pathext_does_not_list(void) {
    path_scratch place;
    fresh(&place, "unlisted");
    spisor_memory_release(touch_file(place.first, "claude.ps1", "", 0644));
    char *search = search_of(&place, ';');
    check_none(found(1, "claude", environment_of("PATH", search, ".EXE;.CMD")), "claude.ps1");
    spisor_memory_release(search);
    discard(&place);
}

static void windows_prefers_a_pathext_match_over_the_bare_name_in_pathext_order(void) {
    path_scratch place;
    fresh(&place, "order");
    spisor_memory_release(touch_file(place.first, "claude", "", 0644));
    spisor_memory_release(touch_file(place.first, "claude.exe", "", 0644));
    char *binary = touch_file(place.first, "claude.cmd", "", 0644);
    char *search = search_of(&place, ';');
    check_found(found(1, "claude", environment_of("PATH", search, ".CMD;.EXE")), binary, "PATHEXT order");
    spisor_memory_release(search);
    spisor_memory_release(binary);
    discard(&place);
}

static void windows_takes_the_bare_name_too(void) {
    path_scratch place;
    fresh(&place, "bare");
    char *binary = touch_file(place.first, "claude", "", 0644);
    char *search = search_of(&place, ';');
    check_found(found(1, "claude", environment_of("PATH", search, NULL)), binary, "the bare name");
    spisor_memory_release(search);
    spisor_memory_release(binary);
    discard(&place);
}

/** One on_path call's status, answer and refusal, released by release_call. */
typedef struct on_path_call {
    spisor_status status;
    runtime_detect_on_path_answer answer;
    spisor_refusal refusal;
} on_path_call;

static on_path_call on_path_through(const char *binary, const runtime_variable *environment, size_t count) {
    const runtime_detect_definition *detect = runtime_detect_definition_of();
    runtime_detect_on_path_argument argument = SPISOR_INIT(runtime_detect_on_path_argument);
    on_path_call call = {SPISOR_OK, SPISOR_INIT(runtime_detect_on_path_answer), SPISOR_INIT(spisor_refusal)};
    argument.binary = binary;
    argument.environment = environment;
    argument.environment_count = count;
    call.status = detect->on_path(detect->context, &argument, &call.answer, &call.refusal);
    return call;
}

static on_path_call on_path(const char *search) {
    runtime_variable environment[] = {{"PATH", search}, {"PATHEXT", ".EXE;.CMD"}};
    return on_path_through("claude", environment, 2);
}

static void release_call(on_path_call *call) {
    const runtime_detect_definition *detect = runtime_detect_definition_of();
    detect->release_answer(detect->context, &call->answer);
    detect->release_refusal(detect->context, &call->refusal);
}

#if defined(_WIN32)
#define MARKING_PROGRAM_NAME "claude.cmd"
#define MARKING_PROGRAM_FORMAT "@echo ran> \"%s\"\r\n"
#else
#define MARKING_PROGRAM_NAME "claude"
#define MARKING_PROGRAM_FORMAT "#!/bin/sh\necho ran > '%s'\n"
#endif

/** The service finds the program on PATH and never runs it: the program would leave a marker file. */
static void the_service_finds_the_binary_and_launches_nothing(void) {
    path_scratch place;
    fresh(&place, "service");
    char *marker = joined_path(place.base, "ran");
    char program[2048];
    snprintf(program, sizeof program, MARKING_PROGRAM_FORMAT, marker);
    char *binary = touch_file(place.first, MARKING_PROGRAM_NAME, program, 0755);
    on_path_call call = on_path(place.first);
    CHECK(call.status == SPISOR_OK);
    check_text(call.answer.path, binary, "the found path");
    CHECK(spisor_probe_file(marker) != SPISOR_FILE_PRESENT);
    release_call(&call);
    CHECK(call.answer.path == NULL && call.answer.storage == NULL);
    call = on_path(place.second);
    CHECK(call.status == SPISOR_NONE && call.answer.path == NULL && call.answer.storage == NULL);
    release_call(&call);
    spisor_memory_release(binary);
    spisor_memory_release(marker);
    discard(&place);
}

static void the_service_refuses_an_argument_without_a_binary_or_a_whole_environment(void) {
    runtime_variable unnamed[] = {{NULL, "x"}};
    on_path_call call = on_path_through(NULL, NULL, 0);
    check_refused(call.status, SPISOR_MALFORMED, &call.refusal, "binary", "no binary");
    release_call(&call);
    call = on_path_through("", NULL, 0);
    check_refused(call.status, SPISOR_MALFORMED, &call.refusal, "binary", "an empty binary");
    release_call(&call);
    call = on_path_through("claude", unnamed, 1);
    check_refused(call.status, SPISOR_MALFORMED, &call.refusal, "environment variable", "a variable without a name");
    release_call(&call);
    call = on_path_through("claude", NULL, 1);
    check_refused(call.status, SPISOR_MALFORMED, &call.refusal, "environment variable", "a count without variables");
    release_call(&call);
}

static void the_service_reads_no_path_as_nothing_found(void) {
    on_path_call call = on_path_through("claude", NULL, 0);
    CHECK(call.status == SPISOR_NONE);
    release_call(&call);
}

int main(void) {
#if !defined(_WIN32)
    posix_finds_an_executable_file();
    posix_skips_a_same_named_file_without_an_execute_bit();
    posix_answers_nothing_when_only_a_non_executable_file_matches();
#endif
    posix_does_not_treat_a_directory_as_an_executable();
    posix_ignores_a_path_entry_that_does_not_exist();
    windows_finds_claude_cmd_through_pathext();
    windows_matches_the_name_and_the_extension_ignoring_case();
    windows_defaults_pathext_when_it_is_unset();
    windows_does_not_take_an_extension_pathext_does_not_list();
    windows_prefers_a_pathext_match_over_the_bare_name_in_pathext_order();
    windows_takes_the_bare_name_too();
    the_service_finds_the_binary_and_launches_nothing();
    the_service_refuses_an_argument_without_a_binary_or_a_whole_environment();
    the_service_reads_no_path_as_nothing_found();
    return check_summary("detect_test");
}
