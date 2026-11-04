#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog_call.h"
#include "check.h"
#include "loader.h"
#include "service_scratch.h"

static const char *VALID_STANDARD =
    "{\"application\": {\"id\": \"claude-code\", \"label\": \"Claude Code\", \"binary\": \"claude\","
    " \"home\": {\"envOverride\": \"CLAUDE_CONFIG_DIR\", \"candidates\": [\"~/.claude\"]},"
    " \"packages\": {\"npm\": \"@anthropic-ai/claude-code\", \"winget\": {\"name\": \"Anthropic.ClaudeCode\","
    " \"source\": \"winget\"}}},"
    " \"pathCommands\": {\"requested\": [\"claude\"], \"minimum\": 0}, \"background\": [\"forebay-host\"]}";

static cJSON *valid_standard(void) {
    return parsed(VALID_STANDARD);
}

static cJSON *member_at(cJSON *standard, const char *first, const char *second) {
    cJSON *found = cJSON_GetObjectItemCaseSensitive(standard, first);
    return second == NULL ? found : cJSON_GetObjectItemCaseSensitive(found, second);
}

static cJSON *application_of(cJSON *standard) {
    return member_at(standard, "application", NULL);
}

static cJSON *home_of(cJSON *standard) {
    return member_at(standard, "application", "home");
}

static cJSON *packages_of(cJSON *standard) {
    return member_at(standard, "application", "packages");
}

/** The issues loader.check answers for standard, which it frees. */
static cJSON *issues_of(cJSON *standard) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddItemToObject(request, "standard", standard);
    catalog_answer answer = call_catalog(runtime_loader_check, request);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    cJSON *issues = cJSON_DetachItemFromObject(answer.tree, "issues");
    cJSON_Delete(answer.tree);
    CHECK(cJSON_IsArray(issues));
    return issues;
}

/** Whether the issues' paths, joined by commas, read expected; reported when not. */
static void check_paths(cJSON *standard, const char *expected, const char *label) {
    cJSON *issues = issues_of(standard);
    char paths[2048] = "";
    const cJSON *issue;
    cJSON_ArrayForEach(issue, issues) {
        if (paths[0] != '\0') {
            strncat(paths, ",", sizeof paths - strlen(paths) - 1);
        }
        const char *path = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(issue, "path"));
        strncat(paths, path == NULL ? "(none)" : path, sizeof paths - strlen(paths) - 1);
        CHECK(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(issue, "detail")));
        CHECK(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(issue, "fix")));
    }
    check_text(paths, expected, label);
    cJSON_Delete(issues);
}

static void accepts_a_complete_standard(void) {
    check_paths(valid_standard(), "", "a complete standard");
}

static void refuses_an_application_with_no_binary_naming_the_member(void) {
    cJSON *standard = valid_standard();
    cJSON_DeleteItemFromObject(application_of(standard), "binary");
    check_paths(standard, "/application/binary", "no binary");
}

static void refuses_a_candidate_neither_home_rooted_nor_absolute_naming_its_index(void) {
    cJSON *standard = valid_standard();
    cJSON_ReplaceItemInObject(home_of(standard), "candidates", parsed("[\"~/.claude\", \"relative/dir\"]"));
    cJSON *issues = issues_of(standard);
    CHECK(cJSON_GetArraySize(issues) >= 1);
    const cJSON *first = cJSON_GetArrayItem(issues, 0);
    check_text(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(first, "path")),
               "/application/home/candidates/1", "the candidate's path");
    const char *fix = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(first, "fix"));
    CHECK(fix != NULL && strstr(fix, "~/") != NULL);
    cJSON_Delete(issues);
}

static void accepts_an_absolute_candidate(void) {
    cJSON *standard = valid_standard();
    cJSON_ReplaceItemInObject(home_of(standard), "candidates", parsed("[\"C:/Users/someone/.claude\", \"/opt/claude\"]"));
    check_paths(standard, "", "absolute candidates");
}

static void accepts_an_xdg_subdir_that_is_one_portable_name(void) {
    cJSON *standard = valid_standard();
    cJSON_AddStringToObject(home_of(standard), "xdgSubdir", "opencode");
    check_paths(standard, "", "a portable xdgSubdir");
}

static void refuses_each_xdg_subdir_that_is_no_portable_name(void) {
    const char *values[] = {"\"\"", "\"../x\"", "\"a/b\"", "7"};
    for (size_t index = 0; index < sizeof values / sizeof values[0]; index++) {
        cJSON *standard = valid_standard();
        cJSON_AddItemToObject(home_of(standard), "xdgSubdir", parsed(values[index]));
        check_paths(standard, "/application/home/xdgSubdir", values[index]);
    }
}

static void refuses_a_package_neither_a_name_nor_an_object_with_one_naming_its_method(void) {
    cJSON *standard = valid_standard();
    cJSON_AddNumberToObject(packages_of(standard), "brew", 7);
    cJSON_AddItemToObject(packages_of(standard), "scoop", parsed("{\"source\": \"x\"}"));
    check_paths(standard, "/application/packages/brew,/application/packages/scoop", "bad packages");
}

static void refuses_a_minimum_outside_zero_and_one_and_a_background_that_is_no_list(void) {
    cJSON *standard = valid_standard();
    cJSON_ReplaceItemInObject(standard, "pathCommands", parsed("{\"requested\": [\"claude\"], \"minimum\": 2}"));
    cJSON_ReplaceItemInObject(standard, "background", cJSON_CreateString("forebay-host"));
    check_paths(standard, "/pathCommands/minimum,/background", "minimum and background");
}

static void refuses_a_value_that_is_not_an_object(void) {
    const char *values[] = {"null", "[]", "\"standard\""};
    for (size_t index = 0; index < sizeof values / sizeof values[0]; index++) {
        cJSON *issues = issues_of(parsed(values[index]));
        CHECK(cJSON_GetArraySize(issues) == 1);
        check_text(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(issues, 0), "path")), "",
                   values[index]);
        cJSON_Delete(issues);
    }
}

static void accepts_a_package_object_with_options_and_with_extra_members(void) {
    cJSON *standard = valid_standard();
    cJSON_AddItemToObject(packages_of(standard), "brew", parsed("{\"name\": \"claude\", \"options\": {\"tap\": \"x\"}}"));
    cJSON_AddItemToObject(packages_of(standard), "scoop", parsed("{\"name\": \"claude\", \"bucket\": \"extras\"}"));
    check_paths(standard, "", "package objects");
}

static void refuses_package_options_that_are_not_an_object_naming_the_method(void) {
    cJSON *standard = valid_standard();
    cJSON_AddItemToObject(packages_of(standard), "brew", parsed("{\"name\": \"claude\", \"options\": \"x\"}"));
    check_paths(standard, "/application/packages/brew", "options that are no object");
}

static void refuses_a_method_id_that_is_not_portable(void) {
    cJSON *standard = valid_standard();
    cJSON_AddStringToObject(packages_of(standard), "", "x");
    cJSON_AddStringToObject(packages_of(standard), "a b", "y");
    check_paths(standard, "/application/packages/,/application/packages/a b", "method ids");
}

static void refuses_each_binary_that_is_no_portable_name(void) {
    const char *binaries[] = {"../x", "a/b", "", "-x", "a b"};
    for (size_t index = 0; index < sizeof binaries / sizeof binaries[0]; index++) {
        cJSON *standard = valid_standard();
        cJSON_ReplaceItemInObject(application_of(standard), "binary", cJSON_CreateString(binaries[index]));
        check_paths(standard, "/application/binary", binaries[index]);
    }
}

static void refuses_each_requested_list_that_is_no_list_of_unique_portable_names(void) {
    const char *lists[] = {"[\"../x\"]", "[\"a/b\"]", "[\"claude\", \"claude\"]", "[\"\"]"};
    for (size_t index = 0; index < sizeof lists / sizeof lists[0]; index++) {
        cJSON *standard = valid_standard();
        cJSON_ReplaceItemInObject(member_at(standard, "pathCommands", NULL), "requested", parsed(lists[index]));
        check_paths(standard, "/pathCommands/requested", lists[index]);
    }
}

static void names_a_binary_in_its_detail_as_json_writes_it(void) {
    cJSON *standard = valid_standard();
    cJSON_ReplaceItemInObject(application_of(standard), "binary", cJSON_CreateString("a/b"));
    cJSON *issues = issues_of(standard);
    check_text(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(issues, 0), "detail")),
               "the binary \"a/b\" is not a portable file name", "the binary's detail");
    cJSON_Delete(issues);
}

typedef struct home_scratch {
    char base[1024];
    char *user;
} home_scratch;

static void fresh_home(home_scratch *place, const char *label) {
    scratch_in(place->base, sizeof place->base, "loader", label);
    place->user = joined_path(place->base, "user");
    CHECK(spisor_make_directories(place->user) == 1);
}

static cJSON *user_environment(const home_scratch *place) {
    cJSON *environment = cJSON_CreateObject();
    cJSON_AddStringToObject(environment, "HOME", place->user);
    cJSON_AddStringToObject(environment, "USERPROFILE", place->user);
    return environment;
}

static const char *CLAUDE_HOME =
    "{\"envOverride\": \"HUB_CLAUDE_DIR\", \"nativeEnv\": \"CLAUDE_CONFIG_DIR\", \"candidates\": [\"~/.claude\", "
    "\"~/.config/claude\"]}";
static const char *OPENCODE_HOME =
    "{\"envOverride\": \"HUB_OPENCODE_DIR\", \"nativeEnv\": \"OPENCODE_CONFIG_DIR\", \"xdgSubdir\": \"opencode\","
    " \"candidates\": [\"~/.config/opencode\", \"~/.opencode\"]}";

static catalog_answer home_answer(const char *home, cJSON *environment) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddItemToObject(request, "home", parsed(home));
    cJSON_AddItemToObject(request, "environment", environment);
    return call_catalog(runtime_loader_home, request);
}

static void check_home(const char *home, cJSON *environment, const char *expected, const char *source) {
    catalog_answer answer = home_answer(home, environment);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    check_text(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(answer.tree, "home")), expected, source);
    check_text(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(answer.tree, "source")), source, "the source");
    cJSON_Delete(answer.tree);
}

static void check_home_refused(const char *home, cJSON *environment, const char *named, const char *label) {
    catalog_answer answer = home_answer(home, environment);
    const char *message = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(answer.tree, "message"));
    const char *fix = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(answer.tree, "fix"));
    if (answer.status != SPISOR_STATUS_FAILURE || message == NULL || strstr(message, named) == NULL || fix == NULL ||
        fix[0] == '\0') {
        fprintf(stderr, "%s: expected a refusal naming %s, got status %d %s\n", label, named, (int)answer.status,
                message == NULL ? "(no message)" : message);
        check_failures++;
    }
    cJSON_Delete(answer.tree);
}

static cJSON *with_variable(cJSON *environment, const char *name, const char *value) {
    cJSON_AddStringToObject(environment, name, value);
    return environment;
}

static void takes_the_override_variable_first(void) {
    home_scratch place;
    fresh_home(&place, "override");
    char *override = joined_path(place.base, "override");
    char *native = joined_path(place.base, "native");
    cJSON *environment = with_variable(with_variable(user_environment(&place), "HUB_CLAUDE_DIR", override),
                                       "CLAUDE_CONFIG_DIR", native);
    check_home(CLAUDE_HOME, environment, override, "override");
    free(override);
    free(native);
    free(place.user);
}

static void takes_the_native_variable_when_there_is_no_override(void) {
    home_scratch place;
    fresh_home(&place, "native");
    char *native = joined_path(place.base, "native");
    check_home(CLAUDE_HOME, with_variable(user_environment(&place), "CLAUDE_CONFIG_DIR", native), native, "native");
    free(native);
    free(place.user);
}

static void treats_an_empty_variable_as_unset(void) {
    home_scratch place;
    fresh_home(&place, "empty");
    char *native = joined_path(place.base, "native");
    cJSON *environment =
        with_variable(with_variable(user_environment(&place), "HUB_CLAUDE_DIR", ""), "CLAUDE_CONFIG_DIR", native);
    check_home(CLAUDE_HOME, environment, native, "native");
    free(native);
    free(place.user);
}

static void takes_the_first_candidate_that_exists_with_the_user_home_expanded(void) {
    home_scratch place;
    fresh_home(&place, "candidate");
    char *config = joined_path(place.user, ".config/claude");
    CHECK(spisor_make_directories(config) == 1);
    check_home(CLAUDE_HOME, user_environment(&place), config, "candidate");
    free(config);
    free(place.user);
}

static void falls_back_to_the_first_candidate_when_none_exists_yet(void) {
    home_scratch place;
    fresh_home(&place, "default");
    char *claude = joined_path(place.user, ".claude");
    check_home(CLAUDE_HOME, user_environment(&place), claude, "default");
    free(claude);
    free(place.user);
}

static void does_not_take_a_candidate_that_is_a_file(void) {
    home_scratch place;
    fresh_home(&place, "file");
    free(touch_file(place.user, ".claude", "", 0644));
    char *config = joined_path(place.user, ".config/claude");
    CHECK(spisor_make_directories(config) == 1);
    check_home(CLAUDE_HOME, user_environment(&place), config, "candidate");
    free(config);
    free(place.user);
}

static void refuses_a_relative_override_naming_the_variable_and_its_value(void) {
    home_scratch place;
    fresh_home(&place, "relative");
    check_home_refused(CLAUDE_HOME, with_variable(user_environment(&place), "HUB_CLAUDE_DIR", "relative/dir"),
                       "HUB_CLAUDE_DIR", "the variable");
    check_home_refused(CLAUDE_HOME, with_variable(user_environment(&place), "HUB_CLAUDE_DIR", "relative/dir"),
                       "relative/dir", "its value");
    free(place.user);
}

static void takes_the_xdg_subdirectory_after_the_native_variable_and_before_the_candidates(void) {
    home_scratch place;
    fresh_home(&place, "xdg");
    char *xdg = joined_path(place.base, "xdg");
    char *opencode = joined_path(place.user, ".opencode");
    CHECK(spisor_make_directories(opencode) == 1);
    char *expected = joined_path(xdg, "opencode");
    check_home(OPENCODE_HOME, with_variable(user_environment(&place), "XDG_CONFIG_HOME", xdg), expected, "xdg");
    free(expected);
    free(opencode);
    free(xdg);
    free(place.user);
}

static void lets_the_native_variable_win_over_xdg_config_home(void) {
    home_scratch place;
    fresh_home(&place, "xdgnative");
    char *xdg = joined_path(place.base, "xdg");
    char *native = joined_path(place.base, "native");
    cJSON *environment =
        with_variable(with_variable(user_environment(&place), "XDG_CONFIG_HOME", xdg), "OPENCODE_CONFIG_DIR", native);
    check_home(OPENCODE_HOME, environment, native, "native");
    free(xdg);
    free(native);
    free(place.user);
}

static void ignores_a_relative_or_empty_xdg_config_home(void) {
    home_scratch place;
    fresh_home(&place, "xdgrelative");
    char *expected = joined_path(place.user, ".config/opencode");
    check_home(OPENCODE_HOME, with_variable(user_environment(&place), "XDG_CONFIG_HOME", "relative"), expected,
               "default");
    check_home(OPENCODE_HOME, with_variable(user_environment(&place), "XDG_CONFIG_HOME", ""), expected, "default");
    free(expected);
    free(place.user);
}

static void ignores_xdg_config_home_for_a_home_with_no_xdg_subdir(void) {
    home_scratch place;
    fresh_home(&place, "noxdg");
    char *xdg = joined_path(place.base, "xdg");
    char *expected = joined_path(place.user, ".claude");
    check_home(CLAUDE_HOME, with_variable(user_environment(&place), "XDG_CONFIG_HOME", xdg), expected, "default");
    free(expected);
    free(xdg);
    free(place.user);
}

static void refuses_a_relative_native_variable(void) {
    home_scratch place;
    fresh_home(&place, "relativenative");
    check_home_refused(CLAUDE_HOME, with_variable(user_environment(&place), "CLAUDE_CONFIG_DIR", "x"),
                       "CLAUDE_CONFIG_DIR", "a relative native variable");
    free(place.user);
}

static void refuses_a_home_candidate_with_no_user_home_to_expand_against(void) {
    check_home_refused(CLAUDE_HOME, cJSON_CreateObject(), "HOME", "no user home");
}

static void refuses_a_home_that_is_no_standard_home(void) {
    catalog_answer answer = home_answer("{\"candidates\": []}", cJSON_CreateObject());
    CHECK(answer.status == SPISOR_STATUS_FAILURE);
    cJSON_Delete(answer.tree);
}

static catalog_answer installed_answer(const char *cache, const char *id, const char *version) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(cJSON_AddObjectToObject(request, "places"), "cache", cache);
    cJSON_AddStringToObject(request, "id", id);
    cJSON_AddStringToObject(request, "version", version);
    return call_catalog(runtime_loader_installed, request);
}

static void write_manifest(const char *cache, const char *text) {
    char *bundle = joined_path(cache, "bundles/claude-loader/1.0.0");
    CHECK(spisor_make_directories(bundle) == 1);
    free(touch_file(bundle, "host.json", text, 0644));
    free(bundle);
}

static void reads_the_standard_an_installed_bundle_carries(void) {
    char cache[1024];
    scratch_in(cache, sizeof cache, "loader", "installed");
    write_manifest(cache, VALID_STANDARD);
    catalog_answer answer = installed_answer(cache, "claude-loader", "1.0.0");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    const cJSON *standard = cJSON_GetObjectItemCaseSensitive(answer.tree, "standard");
    check_text(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(
                   cJSON_GetObjectItemCaseSensitive(standard, "application"), "binary")),
               "claude", "the installed standard's binary");
    cJSON_Delete(answer.tree);
}

static void refuses_an_installed_bundle_that_carries_no_standard(void) {
    char cache[1024];
    scratch_in(cache, sizeof cache, "loader", "uninstalled");
    catalog_answer answer = installed_answer(cache, "claude-loader", "1.0.0");
    const char *message = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(answer.tree, "message"));
    CHECK(answer.status == SPISOR_STATUS_FAILURE && message != NULL && strstr(message, "host.json") != NULL &&
          strstr(message, "could not be read") != NULL);
    cJSON_Delete(answer.tree);
    write_manifest(cache, "{\"application\": {}}");
    answer = installed_answer(cache, "claude-loader", "1.0.0");
    message = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(answer.tree, "message"));
    CHECK(answer.status == SPISOR_STATUS_FAILURE && message != NULL &&
          strstr(message, "is not a loader standard: /application/id") != NULL);
    cJSON_Delete(answer.tree);
    answer = installed_answer(cache, "../claude-loader", "1.0.0");
    CHECK(answer.status == SPISOR_STATUS_FAILURE);
    cJSON_Delete(answer.tree);
}

int main(void) {
    accepts_a_complete_standard();
    refuses_an_application_with_no_binary_naming_the_member();
    refuses_a_candidate_neither_home_rooted_nor_absolute_naming_its_index();
    accepts_an_absolute_candidate();
    accepts_an_xdg_subdir_that_is_one_portable_name();
    refuses_each_xdg_subdir_that_is_no_portable_name();
    refuses_a_package_neither_a_name_nor_an_object_with_one_naming_its_method();
    refuses_a_minimum_outside_zero_and_one_and_a_background_that_is_no_list();
    refuses_a_value_that_is_not_an_object();
    accepts_a_package_object_with_options_and_with_extra_members();
    refuses_package_options_that_are_not_an_object_naming_the_method();
    refuses_a_method_id_that_is_not_portable();
    refuses_each_binary_that_is_no_portable_name();
    refuses_each_requested_list_that_is_no_list_of_unique_portable_names();
    names_a_binary_in_its_detail_as_json_writes_it();
    takes_the_override_variable_first();
    takes_the_native_variable_when_there_is_no_override();
    treats_an_empty_variable_as_unset();
    takes_the_first_candidate_that_exists_with_the_user_home_expanded();
    falls_back_to_the_first_candidate_when_none_exists_yet();
    does_not_take_a_candidate_that_is_a_file();
    refuses_a_relative_override_naming_the_variable_and_its_value();
    takes_the_xdg_subdirectory_after_the_native_variable_and_before_the_candidates();
    lets_the_native_variable_win_over_xdg_config_home();
    ignores_a_relative_or_empty_xdg_config_home();
    ignores_xdg_config_home_for_a_home_with_no_xdg_subdir();
    refuses_a_relative_native_variable();
    refuses_a_home_candidate_with_no_user_home_to_expand_against();
    refuses_a_home_that_is_no_standard_home();
    reads_the_standard_an_installed_bundle_carries();
    refuses_an_installed_bundle_that_carries_no_standard();
    return check_summary("loader_test");
}
