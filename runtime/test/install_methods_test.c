#include "install_fixture.h"

#include "install_ecosystem.h"
#include "install_methods.h"

#define ITEM "tool"

static catalog_answer detect_with(const install_base *base, const char *manifest_packages_json) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "places", places_of(base));
    cJSON_AddStringToObject(argument, "item", ITEM);
    cJSON_AddItemToObject(argument, "manifestPackages", parsed(manifest_packages_json));
    return call_catalog(runtime_install_detect, argument);
}

static void expect_detections(const install_base *base, const char *manifest_packages_json, const char *expected_json,
                              const char *label) {
    catalog_answer answer = detect_with(base, manifest_packages_json);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    expect_json(cJSON_GetObjectItemCaseSensitive(answer.tree, "detections"), expected_json, label);
    cJSON_Delete(answer.tree);
}

/** Runs operation of package through method: 1 on success, else 0 with the refusal in *refusal when one is given. */
static int operate_on(const install_base *base, const char *method, const char *operation, const char *package,
                      cJSON **refusal) {
    cJSON *places = places_of(base);
    cJSON *mapped = cJSON_CreateObject();
    cJSON_AddStringToObject(mapped, "name", package);
    cJSON *refused = NULL;
    int done = runtime_install_operate(places, method, operation, mapped, &refused);
    cJSON_Delete(mapped);
    cJSON_Delete(places);
    if (refusal != NULL) {
        *refusal = refused;
    } else {
        cJSON_Delete(refused);
    }
    return done;
}

static void detects_installs_updates_and_removes_a_package_through_the_fake_method(void) {
    install_base base;
    start_install_base(&base, "methods", "lifecycle");
    const char *packages = "{\"fake\": \"left-pad\"}";
    expect_detections(&base, packages,
                      "[{\"method\":\"fake\",\"item\":\"tool\",\"package\":\"left-pad\",\"answer\":{\"installed\":false}}]",
                      "before install");
    CHECK(operate_on(&base, "fake", "install", "left-pad", NULL));
    expect_detections(&base, packages,
                      "[{\"method\":\"fake\",\"item\":\"tool\",\"package\":\"left-pad\",\"answer\":{\"installed\":true,"
                      "\"version\":\"1.0.0\"}}]",
                      "after install");
    CHECK(operate_on(&base, "fake", "update", "left-pad", NULL));
    expect_detections(&base, packages,
                      "[{\"method\":\"fake\",\"item\":\"tool\",\"package\":\"left-pad\",\"answer\":{\"installed\":true,"
                      "\"version\":\"2.0.0\"}}]",
                      "after update");
    CHECK(operate_on(&base, "fake", "remove", "left-pad", NULL));
    expect_detections(&base, packages,
                      "[{\"method\":\"fake\",\"item\":\"tool\",\"package\":\"left-pad\",\"answer\":{\"installed\":false}}]",
                      "after remove");
    finish_install_base(&base);
}

static void answers_a_method_s_own_refusal_as_given(void) {
    install_base base;
    start_install_base(&base, "methods", "refusal");
    cJSON *refusal = NULL;
    CHECK(!operate_on(&base, "fake", "update", "absent", &refusal));
    expect_json(refusal, "{\"message\":\"absent is not installed\",\"fix\":\"install absent first\"}", "own refusal");
    cJSON_Delete(refusal);
    finish_install_base(&base);
}

static void lets_the_user_s_packages_json_override_the_item_s_own_mapping(void) {
    install_base base;
    start_install_base(&base, "methods", "override");
    write_in(base.portable, "packages.json", "{\"tool\": {\"fake\": \"mine/tool\"}}");
    CHECK(operate_on(&base, "fake", "install", "mine/tool", NULL));
    expect_detections(&base, "{\"fake\": \"theirs/tool\"}",
                      "[{\"method\":\"fake\",\"item\":\"tool\",\"package\":\"mine/tool\",\"answer\":{\"installed\":true,"
                      "\"version\":\"1.0.0\"}}]",
                      "override");
    finish_install_base(&base);
}

static void asks_only_the_methods_in_both_the_mapping_and_the_table(void) {
    install_base base;
    start_install_base(&base, "methods", "both");
    expect_detections(&base, "{\"npm\": \"tool\"}", "[]", "no built-in method");
    finish_install_base(&base);
}

static void refuses_an_operation_no_built_in_method_serves(void) {
    install_base base;
    start_install_base(&base, "methods", "unserved");
    cJSON *refusal = NULL;
    CHECK(!operate_on(&base, "apt", "install", "git", &refusal));
    CHECK(refusal != NULL && strstr(text_at(refusal, "message"), "apt") != NULL && text_at(refusal, "fix") != NULL);
    cJSON_Delete(refusal);
    finish_install_base(&base);
}

static void refuses_a_detect_with_no_usable_places(void) {
    cJSON *argument = parsed("{\"places\": {\"portable\": \"relative\"}, \"item\": \"tool\"}");
    catalog_answer answer = call_catalog(runtime_install_detect, argument);
    expect_refusal(answer, "places", "no places");
    cJSON_Delete(answer.tree);
}

int main(void) {
    detects_installs_updates_and_removes_a_package_through_the_fake_method();
    answers_a_method_s_own_refusal_as_given();
    lets_the_user_s_packages_json_override_the_item_s_own_mapping();
    asks_only_the_methods_in_both_the_mapping_and_the_table();
    refuses_an_operation_no_built_in_method_serves();
    refuses_a_detect_with_no_usable_places();
    return check_summary("install_methods_test");
}
