#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog_call.h"
#include "check.h"
#include "on_path.h"
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
    free(place->first);
    free(place->second);
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
    char *search = malloc(size);
    snprintf(search, size, "%s%c%s", place->first, separator, place->second);
    return search;
}

/** What runtime_find_on_path answers for binary, the environment freed. */
static char *found(int windows, const char *binary, cJSON *environment) {
    char *answer = runtime_find_on_path(windows, binary, environment);
    cJSON_Delete(environment);
    return answer;
}

static void check_found(char *answer, const char *expected, const char *label) {
    check_text(answer, expected, label);
    free(answer);
}

static void check_none(char *answer, const char *label) {
    if (answer != NULL) {
        fprintf(stderr, "%s: expected nothing, got %s\n", label, answer);
        check_failures++;
    }
    free(answer);
}

#if !defined(_WIN32)
static void posix_finds_an_executable_file(void) {
    path_scratch place;
    fresh(&place, "executable");
    char *binary = touch_file(place.second, "claude", "", 0755);
    char *search = search_of(&place, ':');
    check_found(found(0, "claude", environment_of("PATH", search, NULL)), binary, "an executable file");
    free(search);
    free(binary);
    discard(&place);
}

static void posix_skips_a_same_named_file_without_an_execute_bit(void) {
    path_scratch place;
    fresh(&place, "skips");
    free(touch_file(place.first, "claude", "", 0644));
    char *executable = touch_file(place.second, "claude", "", 0755);
    char *search = search_of(&place, ':');
    check_found(found(0, "claude", environment_of("PATH", search, NULL)), executable, "the executable one");
    free(search);
    free(executable);
    discard(&place);
}

static void posix_answers_nothing_when_only_a_non_executable_file_matches(void) {
    path_scratch place;
    fresh(&place, "plain");
    free(touch_file(place.first, "claude", "", 0644));
    char *search = search_of(&place, ':');
    check_none(found(0, "claude", environment_of("PATH", search, NULL)), "a non-executable file");
    free(search);
    discard(&place);
}
#endif

static void posix_does_not_treat_a_directory_as_an_executable(void) {
    path_scratch place;
    fresh(&place, "directory");
    make_directory_in(place.first, "claude");
    char *search = search_of(&place, ':');
    check_none(found(0, "claude", environment_of("PATH", search, NULL)), "a directory");
    free(search);
    discard(&place);
}

static void posix_ignores_a_path_entry_that_does_not_exist(void) {
    path_scratch place;
    fresh(&place, "missing");
    char *missing = joined_path(place.base, "missing");
    check_none(found(0, "claude", environment_of("PATH", missing, NULL)), "a missing entry");
    free(missing);
    discard(&place);
}

static void windows_finds_claude_cmd_through_pathext(void) {
    path_scratch place;
    fresh(&place, "pathext");
    char *binary = touch_file(place.first, "claude.cmd", "", 0644);
    char *search = search_of(&place, ';');
    check_found(found(1, "claude", environment_of("Path", search, ".EXE;.CMD")), binary, "claude.cmd");
    free(search);
    free(binary);
    discard(&place);
}

static void windows_matches_the_name_and_the_extension_ignoring_case(void) {
    path_scratch place;
    fresh(&place, "case");
    char *binary = touch_file(place.first, "CLAUDE.Exe", "", 0644);
    char *search = search_of(&place, ';');
    check_found(found(1, "claude", environment_of("PATH", search, ".exe")), binary, "CLAUDE.Exe");
    free(search);
    free(binary);
    discard(&place);
}

static void windows_defaults_pathext_when_it_is_unset(void) {
    path_scratch place;
    fresh(&place, "default");
    char *binary = touch_file(place.second, "claude.bat", "", 0644);
    char *search = search_of(&place, ';');
    check_found(found(1, "claude", environment_of("PATH", search, NULL)), binary, "claude.bat");
    free(search);
    free(binary);
    discard(&place);
}

static void windows_does_not_take_an_extension_pathext_does_not_list(void) {
    path_scratch place;
    fresh(&place, "unlisted");
    free(touch_file(place.first, "claude.ps1", "", 0644));
    char *search = search_of(&place, ';');
    check_none(found(1, "claude", environment_of("PATH", search, ".EXE;.CMD")), "claude.ps1");
    free(search);
    discard(&place);
}

static void windows_prefers_a_pathext_match_over_the_bare_name_in_pathext_order(void) {
    path_scratch place;
    fresh(&place, "order");
    free(touch_file(place.first, "claude", "", 0644));
    free(touch_file(place.first, "claude.exe", "", 0644));
    char *binary = touch_file(place.first, "claude.cmd", "", 0644);
    char *search = search_of(&place, ';');
    check_found(found(1, "claude", environment_of("PATH", search, ".CMD;.EXE")), binary, "PATHEXT order");
    free(search);
    free(binary);
    discard(&place);
}

static void windows_takes_the_bare_name_too(void) {
    path_scratch place;
    fresh(&place, "bare");
    char *binary = touch_file(place.first, "claude", "", 0644);
    char *search = search_of(&place, ';');
    check_found(found(1, "claude", environment_of("PATH", search, NULL)), binary, "the bare name");
    free(search);
    free(binary);
    discard(&place);
}

static cJSON *on_path_request(const char *search) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "binary", "claude");
    cJSON *environment = cJSON_AddObjectToObject(request, "environment");
    cJSON_AddStringToObject(environment, "PATH", search);
#if defined(_WIN32)
    cJSON_AddStringToObject(environment, "PATHEXT", ".EXE;.CMD");
#endif
    return request;
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
    catalog_answer answer = call_catalog(runtime_detect_on_path, on_path_request(place.first));
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    check_text(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(answer.tree, "path")), binary, "the found path");
    CHECK(!spisor_file_exists(marker));
    cJSON_Delete(answer.tree);
    answer = call_catalog(runtime_detect_on_path, on_path_request(place.second));
    CHECK(answer.status == SPISOR_STATUS_SUCCESS && answer.tree != NULL &&
          cJSON_GetObjectItemCaseSensitive(answer.tree, "path") == NULL);
    cJSON_Delete(answer.tree);
    free(binary);
    free(marker);
    discard(&place);
}

static void the_service_refuses_an_argument_without_a_binary_or_an_environment(void) {
    cJSON *request = on_path_request("x");
    cJSON_DeleteItemFromObject(request, "binary");
    catalog_answer answer = call_catalog(runtime_detect_on_path, request);
    CHECK(answer.status == SPISOR_STATUS_FAILURE &&
          cJSON_IsString(cJSON_GetObjectItemCaseSensitive(answer.tree, "fix")));
    cJSON_Delete(answer.tree);
    request = on_path_request("x");
    cJSON_ReplaceItemInObject(request, "environment", cJSON_CreateString("PATH"));
    answer = call_catalog(runtime_detect_on_path, request);
    CHECK(answer.status == SPISOR_STATUS_FAILURE);
    cJSON_Delete(answer.tree);
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
    the_service_refuses_an_argument_without_a_binary_or_an_environment();
    return check_summary("detect_test");
}
