#include <stdio.h>
#include <string.h>

#include "catalog_call.h"
#include "install_warnings.h"

static catalog_answer warnings_of(const char *manifest_json) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "manifest", parsed(manifest_json));
    return call_catalog(runtime_install_warnings, argument);
}

static void expect_contains_text(const char *text, const char *part, const char *label) {
    if (text == NULL || strstr(text, part) == NULL) {
        fprintf(stderr, "%s: expected \"%s\" in \"%s\"\n", label, part, text == NULL ? "(none)" : text);
        check_failures++;
    }
}

static const cJSON *warning_list(const catalog_answer answer) {
    return cJSON_GetObjectItemCaseSensitive(answer.tree, "warnings");
}

static void warns_for_a_dependency_by_name(void) {
    catalog_answer answer = warnings_of("{\"dependencies\":[{\"importName\":\"left-pad\",\"name\":\"left-pad\"}]}");
    CHECK(answer.status == 0);
    CHECK(cJSON_GetArraySize(warning_list(answer)) == 1);
    const cJSON *warning = cJSON_GetArrayItem(warning_list(answer), 0);
    check_text(text_at(warning, "kind"), "named-dependency", "kind");
    check_text(text_at(warning, "subject"), "left-pad", "subject");
    expect_contains_text(text_at(warning, "detail"), "can break it", "says a rename can break it");
    expect_contains_text(text_at(warning, "detail"), "takes that name", "says whoever takes the name can replace it");
    cJSON_Delete(answer.tree);
}

static void warns_for_a_namespaced_service_with_no_provider(void) {
    catalog_answer answer =
        warnings_of("{\"services\":{\"consumes\":[\"github:12/files:read\",{\"id\":\"github:12/files:write\"}]}}");
    CHECK(answer.status == 0);
    CHECK(cJSON_GetArraySize(warning_list(answer)) == 2);
    const cJSON *warning = cJSON_GetArrayItem(warning_list(answer), 1);
    check_text(text_at(warning, "kind"), "unbound-service", "kind");
    check_text(text_at(warning, "subject"), "github:12/files:write", "subject");
    expect_contains_text(text_at(warning, "detail"), "takes that name", "says whoever takes the name can replace it");
    cJSON_Delete(answer.tree);
}

static void says_nothing_for_a_dependency_by_id(void) {
    catalog_answer answer =
        warnings_of("{\"dependencies\":[{\"importName\":\"left-pad\",\"id\":\"github:5\",\"commit\":\"abc\"}]}");
    CHECK(answer.status == 0);
    CHECK(cJSON_GetArraySize(warning_list(answer)) == 0);
    cJSON_Delete(answer.tree);
}

static void says_nothing_for_a_bare_shared_service_or_a_bound_one(void) {
    catalog_answer answer = warnings_of(
        "{\"services\":{\"consumes\":[\"files\",{\"id\":\"files\"},{\"id\":\"github:12/files\",\"provider\":\"github:12\"}]}}");
    CHECK(answer.status == 0);
    CHECK(cJSON_GetArraySize(warning_list(answer)) == 0);
    cJSON_Delete(answer.tree);
}

static void says_nothing_for_a_manifest_with_neither(void) {
    catalog_answer answer = warnings_of("{}");
    CHECK(answer.status == 0);
    CHECK(cJSON_GetArraySize(warning_list(answer)) == 0);
    cJSON_Delete(answer.tree);
}

static void refuses_a_call_without_a_manifest_object(void) {
    cJSON *argument = cJSON_CreateObject();
    expect_refusal(call_catalog(runtime_install_warnings, argument), "manifest", "no manifest");
}

int main(void) {
    warns_for_a_dependency_by_name();
    warns_for_a_namespaced_service_with_no_provider();
    says_nothing_for_a_dependency_by_id();
    says_nothing_for_a_bare_shared_service_or_a_bound_one();
    says_nothing_for_a_manifest_with_neither();
    refuses_a_call_without_a_manifest_object();
    return check_summary("install_warnings_test");
}
