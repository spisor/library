#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog.h"
#include "catalog_call.h"
#include "catalog_semver.h"
#include "check.h"
#include "spisor/json_kind.h"

#define ITEM_A_2 "{\"a\":{\"kind\":\"plugin\",\"latest\":\"2.0.0\",\"versions\":{\"1.0.0\":{\"files\":{}},\"2.0.0\":{\"files\":{}}}}}"
#define ITEM_A_1_OF_2 \
    "{\"a\":{\"kind\":\"plugin\",\"latest\":\"1.0.0\",\"versions\":{\"1.0.0\":{\"files\":{}},\"2.0.0\":{\"files\":{}}}}}"
#define ITEM_A_1 "{\"a\":{\"kind\":\"plugin\",\"latest\":\"1.0.0\",\"versions\":{\"1.0.0\":{\"files\":{}}}}}"

static catalog_answer resolved(const char *items, const char *enabled, const char *installed, const char *kind) {
    char catalog[2048];
    snprintf(catalog, sizeof catalog, "{\"format\":1,\"items\":%s}", items);
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "catalog", parsed(catalog));
    cJSON_AddItemToObject(argument, "enabled", parsed(enabled));
    cJSON_AddItemToObject(argument, "installed", parsed(installed));
    if (kind != NULL) {
        cJSON_AddStringToObject(argument, "kind", kind);
    }
    return call_catalog(runtime_catalog_resolve, argument);
}

static void expect_resolved(const char *items, const char *enabled, const char *installed, const char *expected_json,
                            const char *label) {
    catalog_answer answer = resolved(items, enabled, installed, NULL);
    cJSON *expected = parsed(expected_json);
    if (answer.status != SPISOR_STATUS_SUCCESS || !cJSON_Compare(answer.tree, expected, 1)) {
        char *printed = answer.tree == NULL ? NULL : cJSON_PrintUnformatted(answer.tree);
        fprintf(stderr, "%s: expected %s, got %s\n", label, expected_json, printed == NULL ? "(none)" : printed);
        cJSON_free(printed);
        check_failures++;
    }
    cJSON_Delete(expected);
    cJSON_Delete(answer.tree);
}

static void it_never_moves_a_pinned_id(void) {
    expect_resolved(ITEM_A_2, "[{\"id\":\"a\",\"pin\":\"1.0.0\"}]", "{\"a\":[\"1.0.0\",\"2.0.0\"]}",
                    "{\"run\":{\"a\":\"1.0.0\"},\"fetch\":[]}", "pinned");
}

static void it_fetches_a_pin_that_is_not_installed_and_runs_nothing_for_it(void) {
    expect_resolved(ITEM_A_2, "[{\"id\":\"a\",\"pin\":\"1.0.0\"}]", "{\"a\":[\"2.0.0\"]}",
                    "{\"run\":{},\"fetch\":[{\"id\":\"a\",\"version\":\"1.0.0\"}]}", "pin not installed");
}

static void it_runs_the_installed_version_and_fetches_a_newer_latest(void) {
    expect_resolved(ITEM_A_2, "[{\"id\":\"a\"}]", "{\"a\":[\"1.0.0\"]}",
                    "{\"run\":{\"a\":\"1.0.0\"},\"fetch\":[{\"id\":\"a\",\"version\":\"2.0.0\"}]}", "newer latest");
    expect_resolved(ITEM_A_2, "[{\"id\":\"a\"}]", "{\"a\":[\"1.0.0\",\"2.0.0\"]}",
                    "{\"run\":{\"a\":\"2.0.0\"},\"fetch\":[]}", "next resolve");
}

static void it_runs_the_newest_installed_version_by_semver(void) {
    expect_resolved("{}", "[{\"id\":\"a\"}]", "{\"a\":[\"1.10.0\",\"1.9.0\",\"1.10.0-beta.1\"]}",
                    "{\"run\":{\"a\":\"1.10.0\"},\"fetch\":[]}", "semver");
}

static void it_runs_the_catalog_s_latest_when_it_is_installed(void) {
    expect_resolved(ITEM_A_1_OF_2, "[{\"id\":\"a\"}]", "{\"a\":[\"1.0.0\",\"2.0.0\"]}",
                    "{\"run\":{\"a\":\"1.0.0\"},\"fetch\":[]}", "latest wins");
}

static void it_fetches_the_latest_of_an_id_with_nothing_installed(void) {
    expect_resolved(ITEM_A_1, "[{\"id\":\"a\"}]", "{}", "{\"run\":{},\"fetch\":[{\"id\":\"a\",\"version\":\"1.0.0\"}]}",
                    "nothing installed");
}

static void it_fetches_nothing_for_an_id_of_another_kind_or_not_listed(void) {
    expect_resolved("{\"runtime\":{\"kind\":\"library\",\"latest\":\"1.0.0\",\"versions\":{\"1.0.0\":{\"files\":{}}}}}",
                    "[{\"id\":\"runtime\"},{\"id\":\"absent\"}]", "{}", "{\"run\":{},\"fetch\":[]}", "other kind");
}

static void it_does_not_fetch_a_pin_the_catalog_does_not_list(void) {
    expect_resolved(ITEM_A_1, "[{\"id\":\"a\",\"pin\":\"9.9.9\"}]", "{}", "{\"run\":{},\"fetch\":[]}", "unlisted pin");
}

static void it_resolves_the_kind_asked_for(void) {
    catalog_answer answer =
        resolved("{\"runtime\":{\"kind\":\"library\",\"latest\":\"1.0.0\",\"versions\":{\"1.0.0\":{\"files\":{}}}}}",
                 "[{\"id\":\"runtime\"}]", "{}", "library");
    cJSON *expected = parsed("{\"run\":{},\"fetch\":[{\"id\":\"runtime\",\"version\":\"1.0.0\"}]}");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS && cJSON_Compare(answer.tree, expected, 1));
    cJSON_Delete(expected);
    cJSON_Delete(answer.tree);
}

static void it_refuses_an_unusable_argument(void) {
    catalog_answer kind = resolved("{}", "[]", "{}", "gadget");
    expect_refusal(kind, "kind", "kind");
    cJSON_Delete(kind.tree);
    catalog_answer enabled = resolved("{}", "[{\"pin\":\"1.0.0\"}]", "{}", NULL);
    expect_refusal(enabled, "enabled", "enabled");
    cJSON_Delete(enabled.tree);
}

static void expect_sorted(const char **versions, size_t count, const char *label) {
    for (size_t index = 1; index < count; index++) {
        if (runtime_catalog_compare_versions(versions[index - 1], versions[index]) >= 0 ||
            runtime_catalog_compare_versions(versions[index], versions[index - 1]) <= 0) {
            fprintf(stderr, "%s: %s does not sort before %s\n", label, versions[index - 1], versions[index]);
            check_failures++;
        }
    }
}

static void it_orders_by_major_minor_and_patch_as_numbers(void) {
    const char *versions[] = {"0.9.9", "1.2.0", "1.9.3", "1.10.0"};
    expect_sorted(versions, 4, "numbers");
}

static void it_orders_a_prerelease_before_its_release(void) {
    const char *versions[] = {"1.0.0-alpha", "1.0.0-beta.2", "1.0.0-beta.10", "1.0.0"};
    expect_sorted(versions, 4, "prerelease");
}

static void it_orders_a_name_that_is_not_a_version_below_every_version(void) {
    const char *versions[] = {"latest", "1.0.0"};
    expect_sorted(versions, 2, "not a version");
}

static void it_answers_the_highest_version_or_none(void) {
    expect_resolved("{}", "[{\"id\":\"a\"}]", "{\"a\":[\"1.0.0\",\"2.0.0-rc.1\",\"1.5.0\"]}",
                    "{\"run\":{\"a\":\"2.0.0-rc.1\"},\"fetch\":[]}", "highest");
    expect_resolved("{}", "[{\"id\":\"a\"}]", "{\"a\":[]}", "{\"run\":{},\"fetch\":[]}", "none");
}

int main(void) {
    it_never_moves_a_pinned_id();
    it_fetches_a_pin_that_is_not_installed_and_runs_nothing_for_it();
    it_runs_the_installed_version_and_fetches_a_newer_latest();
    it_runs_the_newest_installed_version_by_semver();
    it_runs_the_catalog_s_latest_when_it_is_installed();
    it_fetches_the_latest_of_an_id_with_nothing_installed();
    it_fetches_nothing_for_an_id_of_another_kind_or_not_listed();
    it_does_not_fetch_a_pin_the_catalog_does_not_list();
    it_resolves_the_kind_asked_for();
    it_refuses_an_unusable_argument();
    it_orders_by_major_minor_and_patch_as_numbers();
    it_orders_a_prerelease_before_its_release();
    it_orders_a_name_that_is_not_a_version_below_every_version();
    it_answers_the_highest_version_or_none();
    return check_summary("catalog_resolve_test");
}
