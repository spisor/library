#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "definition_fixture.h"
#include "places.h"
#include "runtime/places.h"
#include "cJSON.h"
#include "user_home.h"

typedef struct expected_places {
    const char *portable;
    const char *state;
    const char *cache;
    const char *secrets_file;
} expected_places;

static int resolve(int windows, int macos, const char *home, const char *environment_json, runtime_places *places) {
    cJSON *environment = cJSON_Parse(environment_json);
    int status = runtime_resolve_places(windows, macos, home, environment, places);
    cJSON_Delete(environment);
    return status;
}

static void expect_places(int windows, int macos, const char *home, const char *environment_json,
                          const expected_places *expected, int expected_force) {
    runtime_places places;
    CHECK(resolve(windows, macos, home, environment_json, &places) == RUNTIME_PLACES_OK);
    check_text(places.portable, expected->portable, "portable");
    check_text(places.state, expected->state, "state");
    check_text(places.cache, expected->cache, "cache");
    check_text(places.secrets_file, expected->secrets_file, "secrets file");
    CHECK(places.force_file_store == expected_force);
    runtime_free_places(&places);
}

static const expected_places LINUX_DEFAULTS = {"/u/.spisor", "/u/.local/state/spisor", "/u/.cache/spisor",
                                               "/u/.local/state/spisor/secrets.json"};
static const expected_places MACOS_DEFAULTS = {"/u/.spisor", "/u/Library/Application Support/spisor",
                                               "/u/Library/Caches/spisor",
                                               "/u/Library/Application Support/spisor/secrets.json"};
static const expected_places WINDOWS_DEFAULTS = {"C:\\u\\.spisor", "C:\\L\\spisor", "C:\\L\\spisor\\cache",
                                                 "C:\\L\\spisor\\secrets.json"};
#define WINDOWS_ENVIRONMENT "{\"LOCALAPPDATA\":\"C:\\\\L\"}"

static void linux_defaults(void) {
    expect_places(0, 0, "/u", "{}", &LINUX_DEFAULTS, 0);
}

static void macos_defaults(void) {
    expect_places(0, 1, "/u", "{}", &MACOS_DEFAULTS, 0);
}

static void windows_defaults(void) {
    expect_places(1, 0, "C:\\u", WINDOWS_ENVIRONMENT, &WINDOWS_DEFAULTS, 0);
}

static void absent_environment_is_defaults(void) {
    runtime_places places;
    CHECK(runtime_resolve_places(0, 0, "/u", NULL, &places) == RUNTIME_PLACES_OK);
    check_text(places.state, "/u/.local/state/spisor", "state");
    runtime_free_places(&places);
}

#define UNIX_OVERRIDES \
    "{\"SPISOR_HOME\":\"/p\",\"SPISOR_STATE\":\"/s\",\"SPISOR_CACHE\":\"/c\",\"SPISOR_SECRETS\":\"/f\"}"

static void linux_overrides(void) {
    const expected_places expected = {"/p", "/s", "/c", "/f/secrets.json"};
    expect_places(0, 0, "/u", UNIX_OVERRIDES, &expected, 1);
}

static void macos_overrides(void) {
    const expected_places expected = {"/p", "/s", "/c", "/f/secrets.json"};
    expect_places(0, 1, "/u", UNIX_OVERRIDES, &expected, 1);
}

static void windows_overrides(void) {
    const expected_places expected = {"D:\\p", "D:\\s", "D:\\c", "D:\\f\\secrets.json"};
    expect_places(1, 0, "C:\\u",
                  "{\"LOCALAPPDATA\":\"C:\\\\L\",\"SPISOR_HOME\":\"D:\\\\p\",\"SPISOR_STATE\":\"D:\\\\s\","
                  "\"SPISOR_CACHE\":\"D:\\\\c\",\"SPISOR_SECRETS\":\"D:\\\\f\"}",
                  &expected, 1);
}

static void secrets_file_follows_the_state_override(void) {
    const expected_places expected = {"/u/.spisor", "/s", "/u/.cache/spisor", "/s/secrets.json"};
    expect_places(0, 0, "/u", "{\"SPISOR_STATE\":\"/s\"}", &expected, 0);
}

static void empty_and_relative_overrides_are_ignored(void) {
    expect_places(0, 0, "/u",
                  "{\"SPISOR_HOME\":\"\",\"SPISOR_STATE\":\"rel/s\",\"SPISOR_CACHE\":\"c\",\"SPISOR_SECRETS\":\"\","
                  "\"XDG_STATE_HOME\":\"\",\"XDG_CACHE_HOME\":\"relative\"}",
                  &LINUX_DEFAULTS, 0);
    expect_places(1, 0, "C:\\u",
                  "{\"LOCALAPPDATA\":\"C:\\\\L\",\"SPISOR_HOME\":\"rel\",\"SPISOR_SECRETS\":\"\"}",
                  &WINDOWS_DEFAULTS, 0);
}

static void xdg_is_honoured_on_linux_only(void) {
    const char *environment = "{\"XDG_STATE_HOME\":\"/xs\",\"XDG_CACHE_HOME\":\"/xc\"}";
    const expected_places linux_expected = {"/u/.spisor", "/xs/spisor", "/xc/spisor", "/xs/spisor/secrets.json"};
    expect_places(0, 0, "/u", environment, &linux_expected, 0);
    expect_places(0, 1, "/u", environment, &MACOS_DEFAULTS, 0);
}

static void spisor_override_beats_xdg(void) {
    const expected_places expected = {"/u/.spisor", "/s", "/c", "/s/secrets.json"};
    expect_places(0, 0, "/u",
                  "{\"XDG_STATE_HOME\":\"/xs\",\"XDG_CACHE_HOME\":\"/xc\",\"SPISOR_STATE\":\"/s\","
                  "\"SPISOR_CACHE\":\"/c\"}",
                  &expected, 0);
}

static void windows_without_local_app_data_is_refused(void) {
    runtime_places places;
    CHECK(resolve(1, 0, "C:\\u", "{}", &places) == RUNTIME_PLACES_NO_LOCAL_APP_DATA);
    CHECK(places.portable == NULL && places.state == NULL);
    CHECK(resolve(1, 0, "C:\\u", "{\"LOCALAPPDATA\":\"\"}", &places) == RUNTIME_PLACES_NO_LOCAL_APP_DATA);
    CHECK(resolve(1, 0, "C:\\u", "{\"LOCALAPPDATA\":\"rel\"}", &places) == RUNTIME_PLACES_NO_LOCAL_APP_DATA);
    CHECK(runtime_resolve_places(1, 0, "C:\\u", NULL, &places) == RUNTIME_PLACES_NO_LOCAL_APP_DATA);
}

static void windows_overrides_cover_every_derived_place(void) {
    const expected_places expected = {"C:\\u\\.spisor", "D:\\s", "D:\\c", "D:\\s\\secrets.json"};
    expect_places(1, 0, "C:\\u", "{\"SPISOR_STATE\":\"D:\\\\s\",\"SPISOR_CACHE\":\"D:\\\\c\"}", &expected, 0);
    runtime_places places;
    CHECK(resolve(1, 0, "C:\\u", "{\"SPISOR_STATE\":\"D:\\\\s\"}", &places) == RUNTIME_PLACES_NO_LOCAL_APP_DATA);
    CHECK(resolve(1, 0, "C:\\u", "{\"SPISOR_CACHE\":\"D:\\\\c\"}", &places) == RUNTIME_PLACES_NO_LOCAL_APP_DATA);
}

static const runtime_places_definition *places_definition(void) {
    return runtime_places_definition_of();
}

static spisor_status resolve_with(const char *home, const runtime_variable *environment, size_t count,
                                  runtime_places_answer *answer, spisor_refusal *refusal) {
    const runtime_places_definition *definition = places_definition();
    runtime_places_resolve_argument argument = SPISOR_INIT(runtime_places_resolve_argument);
    argument.home = home;
    argument.environment = environment;
    argument.environment_count = count;
    return definition->resolve(definition->context, &argument, answer, refusal);
}

static void release_both(runtime_places_answer *answer, spisor_refusal *refusal) {
    const runtime_places_definition *definition = places_definition();
    definition->release_answer(definition->context, answer);
    definition->release_refusal(definition->context, refusal);
}

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

#if defined(_WIN32)
#define DEFINITION_HOME "C:\\u"
#define EXPLICIT_HOME "C:\\x"
#define POSIX_HOME "C:\\h"
#define WINDOWS_PROFILE "C:\\w"
#define LOCAL_APP_DATA {"LOCALAPPDATA", "C:\\L"}
#else
#define DEFINITION_HOME "/u"
#define EXPLICIT_HOME "/x"
#define POSIX_HOME "/h"
#define WINDOWS_PROFILE "/w"
#define LOCAL_APP_DATA {"UNUSED", ""}
#endif

static void the_definition_is_one_static_struct(void) {
    const runtime_places_definition *definition = places_definition();
    CHECK(definition != NULL && (const void *)definition == runtime_places_definition_of());
    CHECK(definition != NULL && definition->size == sizeof(runtime_places_definition));
    CHECK(definition != NULL && definition->resolve != NULL && definition->release_answer != NULL &&
          definition->release_refusal != NULL);
}

static void resolves_every_place(void) {
    runtime_variable environment[] = {LOCAL_APP_DATA};
    runtime_places_answer answer = SPISOR_INIT(runtime_places_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    CHECK(resolve_with(DEFINITION_HOME, environment, COUNT(environment), &answer, &refusal) == SPISOR_OK);
    check_text(answer.home, DEFINITION_HOME, "home");
    CHECK(ends_with(answer.portable, ".spisor"));
    CHECK(ends_with(answer.state, "spisor"));
    CHECK(ends_with(answer.cache, "cache") || ends_with(answer.cache, "spisor"));
    CHECK(ends_with(answer.secrets_file, "secrets.json"));
    CHECK(answer.force_file_store == 0 && answer.storage != NULL);
    release_both(&answer, &refusal);
    CHECK(answer.storage == NULL && answer.home == NULL && answer.size == sizeof answer);
}

static void a_secrets_override_forces_the_file_store(void) {
#if defined(_WIN32)
    runtime_variable environment[] = {LOCAL_APP_DATA, {"SPISOR_SECRETS", "D:\\f"}};
    const char *expected = "D:\\f\\secrets.json";
#else
    runtime_variable environment[] = {{"SPISOR_SECRETS", "/f"}};
    const char *expected = "/f/secrets.json";
#endif
    runtime_places_answer answer = SPISOR_INIT(runtime_places_answer);
    CHECK(resolve_with(DEFINITION_HOME, environment, COUNT(environment), &answer, NULL) == SPISOR_OK);
    check_text(answer.secrets_file, expected, "secrets file");
    CHECK(answer.force_file_store == 1);
    release_both(&answer, NULL);
}

static void refuses(const char *home, const runtime_variable *environment, size_t count,
                    const char *detail_part, const char *label) {
    runtime_places_answer answer = SPISOR_INIT(runtime_places_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    check_refused(resolve_with(home, environment, count, &answer, &refusal), SPISOR_MALFORMED, &refusal, detail_part,
                  label);
    CHECK(answer.storage == NULL && answer.home == NULL);
    release_both(&answer, &refusal);
    CHECK(refusal.storage == NULL);
}

static void refuses_a_relative_home(void) {
    runtime_variable environment[] = {LOCAL_APP_DATA};
    refuses("rel", environment, COUNT(environment), "home", "a relative home");
}

static void refuses_a_variable_without_a_name_or_a_value(void) {
    runtime_variable no_name[] = {{NULL, "/p"}};
    runtime_variable no_value[] = {{"SPISOR_HOME", NULL}};
    refuses(DEFINITION_HOME, no_name, 1, "variable", "a variable without a name");
    refuses(DEFINITION_HOME, no_value, 1, "variable", "a variable without a value");
    refuses(DEFINITION_HOME, NULL, 2, "variable", "a count without variables");
}

static void expect_derived_home(const runtime_variable *environment, size_t count, const char *expected) {
    runtime_places_answer answer = SPISOR_INIT(runtime_places_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    CHECK(resolve_with(NULL, environment, count, &answer, &refusal) == SPISOR_OK);
    check_text(answer.home, expected, "derived home");
    release_both(&answer, &refusal);
}

static void derives_the_home_from_home_on_posix_shapes(void) {
    runtime_variable environment[] = {{"HOME", POSIX_HOME}, LOCAL_APP_DATA};
    expect_derived_home(environment, COUNT(environment), POSIX_HOME);
}

static void derives_the_home_from_userprofile_on_windows(void) {
    runtime_variable environment[] = {{"HOME", POSIX_HOME}, {"USERPROFILE", WINDOWS_PROFILE},
                                                  LOCAL_APP_DATA};
#if defined(_WIN32)
    expect_derived_home(environment, COUNT(environment), WINDOWS_PROFILE);
#else
    expect_derived_home(environment, COUNT(environment), POSIX_HOME);
#endif
}

static void refuses_an_environment_with_no_home(void) {
    runtime_variable empty_home[] = {{"HOME", ""}};
    runtime_places_answer answer = SPISOR_INIT(runtime_places_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    refuses(NULL, NULL, 0, RUNTIME_NO_USER_HOME_MESSAGE, "no environment");
    refuses(NULL, empty_home, 1, RUNTIME_NO_USER_HOME_MESSAGE, "an empty HOME");
    resolve_with(NULL, empty_home, 1, &answer, &refusal);
    check_text(refusal.fix, RUNTIME_NO_USER_HOME_FIX, "fix");
    release_both(&answer, &refusal);
}

static void an_empty_userprofile_with_home_set_refuses_on_windows(void) {
    runtime_variable environment[] = {{"HOME", POSIX_HOME}, {"USERPROFILE", ""}, LOCAL_APP_DATA};
#if defined(_WIN32)
    refuses(NULL, environment, COUNT(environment), RUNTIME_NO_USER_HOME_MESSAGE, "an empty USERPROFILE");
#else
    expect_derived_home(environment, COUNT(environment), POSIX_HOME);
#endif
}

static void an_explicit_home_wins_over_the_environment(void) {
    runtime_variable environment[] = {{"HOME", POSIX_HOME}, {"USERPROFILE", WINDOWS_PROFILE},
                                                  LOCAL_APP_DATA};
    runtime_places_answer answer = SPISOR_INIT(runtime_places_answer);
    CHECK(resolve_with(EXPLICIT_HOME, environment, COUNT(environment), &answer, NULL) == SPISOR_OK);
    check_text(answer.home, EXPLICIT_HOME, "home");
    release_both(&answer, NULL);
}

static void a_name_given_twice_is_malformed(void) {
#if defined(_WIN32)
    runtime_variable environment[] = {LOCAL_APP_DATA, {"SPISOR_HOME", "D:\\first"},
                                                  {"SPISOR_HOME", "D:\\second"}};
#else
    runtime_variable environment[] = {{"SPISOR_HOME", "/first"}, {"SPISOR_HOME", "/second"}};
#endif
    runtime_places_answer answer = SPISOR_INIT(runtime_places_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    CHECK(resolve_with(DEFINITION_HOME, environment, COUNT(environment), &answer, &refusal) == SPISOR_MALFORMED);
    CHECK(answer.storage == NULL);
    release_both(&answer, &refusal);
}

static void windows_without_local_app_data_refuses(void) {
#if defined(_WIN32)
    refuses(DEFINITION_HOME, NULL, 0, "LOCALAPPDATA", "no LOCALAPPDATA");
#endif
}

static void refuses_an_argument_or_answer_short_of_its_shape(void) {
    const runtime_places_definition *definition = places_definition();
    runtime_places_resolve_argument argument = SPISOR_INIT(runtime_places_resolve_argument);
    runtime_places_answer answer = SPISOR_INIT(runtime_places_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    argument.home = DEFINITION_HOME;
    argument.size = (uint32_t)offsetof(runtime_places_resolve_argument, environment_count);
    check_refused(definition->resolve(definition->context, &argument, &answer, &refusal), SPISOR_MALFORMED, &refusal,
                  "first shape", "a short argument");
    release_both(&answer, &refusal);
    argument.size = sizeof argument;
    answer.size = (uint32_t)offsetof(runtime_places_answer, storage);
    check_refused(definition->resolve(definition->context, &argument, &answer, &refusal), SPISOR_MALFORMED, &refusal,
                  "first shape", "a short answer");
    release_both(&answer, &refusal);
}

int main(void) {
    linux_defaults();
    macos_defaults();
    windows_defaults();
    absent_environment_is_defaults();
    linux_overrides();
    macos_overrides();
    windows_overrides();
    secrets_file_follows_the_state_override();
    empty_and_relative_overrides_are_ignored();
    xdg_is_honoured_on_linux_only();
    spisor_override_beats_xdg();
    windows_without_local_app_data_is_refused();
    windows_overrides_cover_every_derived_place();
    the_definition_is_one_static_struct();
    resolves_every_place();
    a_secrets_override_forces_the_file_store();
    refuses_a_relative_home();
    refuses_a_variable_without_a_name_or_a_value();
    derives_the_home_from_home_on_posix_shapes();
    derives_the_home_from_userprofile_on_windows();
    refuses_an_environment_with_no_home();
    an_empty_userprofile_with_home_set_refuses_on_windows();
    an_explicit_home_wins_over_the_environment();
    a_name_given_twice_is_malformed();
    windows_without_local_app_data_refuses();
    refuses_an_argument_or_answer_short_of_its_shape();
    return check_summary("places_test");
}
