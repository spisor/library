#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "places.h"
#include "spisor/json_kind.h"

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

static void call_export(const char *request, int32_t *status, cJSON **result) {
    uint8_t *out = NULL;
    size_t out_len = 0;
    *status = runtime_places_resolve((const uint8_t *)request, strlen(request), &out, &out_len);
    *result = out == NULL ? NULL : spisor_parse_json(out, out_len);
    free(out);
}

static void expect_ends_with(const cJSON *result, const char *name, const char *suffix) {
    const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result, name));
    CHECK(value != NULL);
    if (value != NULL) {
        size_t value_length = strlen(value);
        size_t suffix_length = strlen(suffix);
        CHECK(value_length >= suffix_length && strcmp(value + value_length - suffix_length, suffix) == 0);
    }
}

#if defined(_WIN32)
#define EXPORT_REQUEST "{\"home\":\"C:\\\\u\",\"environment\":{\"LOCALAPPDATA\":\"C:\\\\L\"}}"
#else
#define EXPORT_REQUEST "{\"home\":\"/u\",\"environment\":{}}"
#endif

static void exported_call_round_trip(void) {
    int32_t status;
    cJSON *result;
    call_export(EXPORT_REQUEST, &status, &result);
    CHECK(status == SPISOR_STATUS_SUCCESS);
    CHECK(result != NULL);
    if (result != NULL) {
        expect_ends_with(result, "portable", ".spisor");
        expect_ends_with(result, "state", "spisor");
        expect_ends_with(result, "secretsFile", "secrets.json");
        CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(result, "forceFileStore")));
    }
    cJSON_Delete(result);
}

static void exported_call_refuses_a_relative_home(void) {
    int32_t status;
    cJSON *result;
    call_export("{\"home\":\"rel\",\"environment\":{}}", &status, &result);
    CHECK(status != SPISOR_STATUS_SUCCESS);
    cJSON_Delete(result);
}

static void exported_call_refuses_an_environment_that_is_not_an_object(void) {
    static const char *const requests[] = {
        "{\"home\":\"/u\",\"environment\":[\"SPISOR_HOME=/p\"]}",
        "{\"home\":\"/u\",\"environment\":\"SPISOR_HOME=/p\"}",
    };
    for (size_t index = 0; index < sizeof requests / sizeof requests[0]; index++) {
        int32_t status;
        cJSON *result;
        call_export(requests[index], &status, &result);
        const char *message = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result, "message"));
        CHECK(status == SPISOR_STATUS_FAILURE);
        CHECK(message != NULL && strstr(message, "not an object") != NULL);
        cJSON_Delete(result);
    }
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
    exported_call_round_trip();
    exported_call_refuses_a_relative_home();
    exported_call_refuses_an_environment_that_is_not_an_object();
    return check_summary("places_test");
}
