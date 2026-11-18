#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#define CURRENT_PROCESS_ID _getpid()
#else
#include <unistd.h>
#define CURRENT_PROCESS_ID getpid()
#endif

#include "check.h"
#include "spisor/allocation.h"
#include "install_call.h"
#include "scratch_directory.h"
#include "spisor/file.h"

typedef catalog_answer answer;

static char portable[1100];

static void fresh_portable(const char *label) {
    static int sequence = 0;
    snprintf(portable, sizeof portable, RUNTIME_SCRATCH_PATH "/install-%d-%d-%s", (int)CURRENT_PROCESS_ID, sequence++,
             label);
    start_empty_directory(portable);
}

static void write_packages_file(const char *text) {
    char path[1200];
    snprintf(path, sizeof path, "%s/packages.json", portable);
    CHECK(spisor_replace_file(path, (const uint8_t *)text, strlen(text)) == 1);
}

static answer call_with(install_operation operation, const char *arguments_json) {
    cJSON *argument = cJSON_Parse(arguments_json);
    cJSON *places = cJSON_CreateObject();
    cJSON_AddStringToObject(places, "portable", portable);
    cJSON_AddItemToObject(argument, "places", places);
    return install_call(operation, argument);
}

static void finish(answer *result) {
    cJSON_Delete(result->tree);
}

static void expect_member(const answer *result, const char *member, const char *expected_json, const char *label) {
    CHECK(result->status == SPISOR_OK);
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(result->tree, member);
    char *actual = item == NULL ? NULL : cJSON_PrintUnformatted(item);
    check_text(actual, expected_json, label);
    spisor_memory_release(actual);
}

static void expect_status(const answer *result, spisor_status expected, const char *fragment, const char *label) {
    if (result->status != (int32_t)expected) {
        fprintf(stderr, "%s: expected status %d, got status %d\n", label, (int)expected, (int)result->status);
        check_failures++;
        return;
    }
    char *text = result->tree == NULL ? NULL : cJSON_PrintUnformatted(result->tree);
    if (text == NULL || strstr(text, fragment) == NULL) {
        fprintf(stderr, "%s: refusal %s does not mention %s\n", label, text == NULL ? "(none)" : text, fragment);
        check_failures++;
    }
    spisor_memory_release(text);
}

static void expect_refused(const answer *result, const char *fragment, const char *label) {
    expect_status(result, SPISOR_REFUSED, fragment, label);
}

static void expect_malformed(const answer *result, const char *fragment, const char *label) {
    expect_status(result, SPISOR_MALFORMED, fragment, label);
}

static answer packages(const char *arguments_json) {
    return call_with(INSTALL_PACKAGES, arguments_json);
}

static answer plan(const char *arguments_json) {
    return call_with(INSTALL_PLAN, arguments_json);
}

#define NPM_MANIFEST "\"manifestPackages\":{\"npm\":\"@vendor/tool\",\"brew\":{\"name\":\"tool\",\"options\":{\"tap\":\"v/t\"}}}"
#define NPM_PLAN_BASE "\"item\":\"tool\",\"methods\":[\"npm\",\"brew\"]," NPM_MANIFEST
#define NPM_PROVIDER "{\"id\":\"npm-provider\",\"kind\":\"install-method\",\"installMethod\":\"npm\",\"sourceId\":\"a\"}"

static void manifest_alone_is_normalized(void) {
    fresh_portable("manifest");
    answer result = packages("{\"item\":\"tool\"," NPM_MANIFEST "}");
    expect_member(&result, "packages",
                  "{\"npm\":{\"name\":\"@vendor/tool\"},\"brew\":{\"name\":\"tool\",\"options\":{\"tap\":\"v/t\"}}}",
                  "string and object mappings answer in the object form");
    finish(&result);
}

static void absent_file_and_item_add_nothing(void) {
    fresh_portable("absent");
    answer result = packages("{\"item\":\"tool\"}");
    expect_member(&result, "packages", "{}", "no manifest and no file answers empty");
    finish(&result);
    write_packages_file("{\"other\":{\"npm\":\"x\"}}");
    result = packages("{\"item\":\"tool\"," NPM_MANIFEST "}");
    expect_member(&result, "packages",
                  "{\"npm\":{\"name\":\"@vendor/tool\"},\"brew\":{\"name\":\"tool\",\"options\":{\"tap\":\"v/t\"}}}",
                  "another item's entry changes nothing");
    finish(&result);
}

static void user_overrides_one_method_and_keeps_the_others(void) {
    fresh_portable("override");
    write_packages_file("{\"tool\":{\"npm\":{\"name\":\"@mine/tool\",\"options\":{\"registry\":\"https://r\"}},"
                        "\"winget\":\"Vendor.Tool\"}}");
    answer result = packages("{\"item\":\"tool\"," NPM_MANIFEST "}");
    expect_member(&result, "packages",
                  "{\"npm\":{\"name\":\"@mine/tool\",\"options\":{\"registry\":\"https://r\"}},"
                  "\"brew\":{\"name\":\"tool\",\"options\":{\"tap\":\"v/t\"}},\"winget\":{\"name\":\"Vendor.Tool\"}}",
                  "the user's npm replaces the manifest's, brew stays, winget is added");
    finish(&result);
}

static void unreadable_file_is_refused_naming_it(void) {
    const char *broken[] = {"{not json", "[1]", "{\"tool\":[1]}", "{\"tool\":{\"npm\":7}}",
                            "{\"tool\":{\"npm\":{\"name\":\"\"}}}", "{\"tool\":{\"npm\":{\"name\":\"a\",\"options\":3}}}"};
    for (size_t index = 0; index < sizeof broken / sizeof broken[0]; index++) {
        fresh_portable("broken");
        write_packages_file(broken[index]);
        answer result = packages("{\"item\":\"tool\"," NPM_MANIFEST "}");
        expect_refused(&result, "packages.json", broken[index]);
        finish(&result);
    }
}

static void unreadable_file_that_is_a_directory_is_refused(void) {
    fresh_portable("directory");
    char path[1200];
    snprintf(path, sizeof path, "%s/packages.json", portable);
    CHECK(spisor_make_directories(path) == 1);
    answer result = packages("{\"item\":\"tool\"}");
    expect_refused(&result, "packages.json", "a directory in place of the file");
    finish(&result);
}

static void bad_arguments_are_refused(void) {
    fresh_portable("arguments");
    answer result = packages("{}");
    expect_malformed(&result, "item", "no item");
    finish(&result);
    result = packages("{\"item\":\"tool\",\"manifestPackages\":{\"npm\":3}}");
    expect_refused(&result, "manifestPackages", "a bad manifest mapping");
    finish(&result);
    result = packages("{\"item\":\"tool\",\"manifestPackages\":[]}");
    expect_malformed(&result, "manifest packages", "a manifest that is not an object");
    finish(&result);
}

static void installed_provider_is_one_step(void) {
    fresh_portable("installed");
    answer result = plan("{" NPM_PLAN_BASE ",\"chosen\":\"npm\",\"installedProviders\":[\"npm\"],\"catalog\":[" NPM_PROVIDER "]}");
    expect_member(&result, "steps", "[{\"install\":{\"method\":\"npm\",\"package\":{\"name\":\"@vendor/tool\"}}}]",
                  "an installed provider needs no provider step");
    finish(&result);
}

static void missing_provider_is_installed_first(void) {
    fresh_portable("missing");
    answer result = plan("{" NPM_PLAN_BASE ",\"chosen\":\"npm\",\"installedProviders\":[\"brew\"],\"catalog\":["
                         "{\"id\":\"other\",\"kind\":\"install-method\",\"installMethod\":\"brew\"}," NPM_PROVIDER ","
                         "{\"id\":\"npm-late\",\"kind\":\"install-method\",\"installMethod\":\"npm\"}]}");
    expect_member(&result, "steps",
                  "[{\"install-provider\":" NPM_PROVIDER "},"
                  "{\"install\":{\"method\":\"npm\",\"package\":{\"name\":\"@vendor/tool\"}}}]",
                  "the first suitable entry is installed, then the item");
    finish(&result);
}

static void user_mapping_reaches_the_plan(void) {
    fresh_portable("plan-user");
    write_packages_file("{\"tool\":{\"npm\":\"@mine/tool\"}}");
    answer result = plan("{" NPM_PLAN_BASE ",\"chosen\":\"npm\",\"installedProviders\":[\"npm\"]}");
    expect_member(&result, "steps", "[{\"install\":{\"method\":\"npm\",\"package\":{\"name\":\"@mine/tool\"}}}]",
                  "the plan installs the user's package");
    finish(&result);
}

static void no_provider_anywhere_is_refused(void) {
    fresh_portable("no-provider");
    answer result = plan("{" NPM_PLAN_BASE ",\"chosen\":\"npm\",\"installedProviders\":[],\"catalog\":["
                         "{\"id\":\"other\",\"kind\":\"install-method\",\"installMethod\":\"brew\"}]}");
    expect_refused(&result, "\\\"npm\\\"", "no suitable provider names the method");
    finish(&result);
    result = plan("{" NPM_PLAN_BASE ",\"chosen\":\"npm\"}");
    expect_refused(&result, "\\\"npm\\\"", "no catalog at all");
    finish(&result);
}

static void marketplace_is_always_plannable(void) {
    fresh_portable("marketplace");
    answer result = plan("{\"item\":\"tool\",\"methods\":[\"marketplace\",\"npm\"],\"chosen\":\"marketplace\"}");
    expect_member(&result, "steps", "[{\"install\":{\"method\":\"marketplace\"}}]", "no provider and no package");
    finish(&result);
    write_packages_file("{broken");
    result = plan("{\"item\":\"tool\",\"methods\":[\"npm\"],\"chosen\":\"marketplace\"}");
    expect_member(&result, "steps", "[{\"install\":{\"method\":\"marketplace\"}}]",
                  "built in, even unlisted and beside a broken packages file");
    finish(&result);
}

static void unlisted_method_is_refused(void) {
    fresh_portable("unlisted");
    write_packages_file("{\"tool\":{\"apt\":\"tool\"}}");
    answer result = plan("{" NPM_PLAN_BASE ",\"chosen\":\"apt\",\"installedProviders\":[\"apt\"]}");
    expect_refused(&result, "\\\"apt\\\"", "a method the item does not list");
    finish(&result);
}

static void method_without_a_package_is_refused(void) {
    fresh_portable("no-package");
    answer result = plan("{\"item\":\"tool\",\"methods\":[\"apt\"],\"chosen\":\"apt\",\"installedProviders\":[\"apt\"]}");
    expect_refused(&result, "no package", "a listed method with no mapping");
    finish(&result);
}

static void bad_plan_arguments_are_refused(void) {
    fresh_portable("plan-arguments");
    answer result = plan("{\"item\":\"tool\",\"methods\":[3],\"chosen\":\"npm\"}");
    expect_malformed(&result, "methods", "a method that is not text");
    finish(&result);
    result = plan("{\"item\":\"tool\",\"methods\":[\"npm\"]}");
    expect_malformed(&result, "chosen", "no chosen method");
    finish(&result);
}

int main(void) {
    manifest_alone_is_normalized();
    absent_file_and_item_add_nothing();
    user_overrides_one_method_and_keeps_the_others();
    unreadable_file_is_refused_naming_it();
    unreadable_file_that_is_a_directory_is_refused();
    bad_arguments_are_refused();
    installed_provider_is_one_step();
    missing_provider_is_installed_first();
    user_mapping_reaches_the_plan();
    no_provider_anywhere_is_refused();
    marketplace_is_always_plannable();
    unlisted_method_is_refused();
    method_without_a_package_is_refused();
    bad_plan_arguments_are_refused();
    return check_summary("install_test");
}
