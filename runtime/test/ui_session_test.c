#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <string.h>

#include "catalog_call.h"
#include "check.h"
#include "config.h"
#include "runtime/config.h"
#include "runtime/places.h"
#include "runtime/ui.h"
#include "runtime_testing_fixture.h"
#include "spisor/allocation.h"
#include "spisor/json_allocation.h"
#include "tree_snapshot.h"

#define LOADER "test-loader"
#define ALPHA                                                                                                      \
    "{\"id\": \"alpha\", \"version\": \"1.0.0\", \"entry\": \"index.js\", \"config\": {\"defaults\": {\"depth\": 2}}," \
    " \"settings\": {\"depth\": {\"type\": \"number\", \"layer\": \"application\"}, \"token\": {\"type\": \"secret\"}}}"
#define BETA "{\"id\": \"beta\", \"version\": \"2.0.0\", \"entry\": \"index.js\"}"
#define GAMMA "{\"id\": \"gamma\", \"version\": \"3.0.0\", \"entry\": \"index.js\"}"
#define FILE_REFERENCE \
    "{\"plugin.json\": {\"url\": \"plugin.json\", \"sha256\": \"" \
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\", \"size\": 3}}"

/** A home laid out with its places resolved, and the session read from it. */
typedef struct session_home {
    laid_out home;
    spisor_value_tree manifests[3];
    runtime_testing_install installs[3];
    runtime_places_answer places;
    spisor_status status;
    runtime_ui_session_answer answer;
    spisor_refusal refusal;
} session_home;

static const runtime_ui_definition *ui(void) {
    return (const runtime_ui_definition *)runtime_ui_definition_of();
}

static const runtime_places_definition *places_service(void) {
    return (const runtime_places_definition *)runtime_places_definition_of();
}

/** Lays out host_id's home with the plugins of `manifests`, the first given settings, and resolves its places. */
static void lay_out_session_home(session_home *home, const char *label, const char *host_id,
                                 const char *const *manifests, size_t count, const spisor_value *first_settings) {
    runtime_boot_library libraries[2];
    runtime_testing_home_argument argument;
    runtime_places_resolve_argument resolve = SPISOR_INIT(runtime_places_resolve_argument);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    memset(home, 0, sizeof *home);
    home->answer.size = (uint32_t)sizeof home->answer;
    home->refusal.size = (uint32_t)sizeof home->refusal;
    start_parent(label);
    for (size_t index = 0; index < count; index++) {
        home->manifests[index] = tree_of(parsed(manifests[index]));
        home->installs[index].manifest = &home->manifests[index].root;
        home->installs[index].entry_source = entry_source;
        home->installs[index].settings = index == 0 ? first_settings : NULL;
    }
    default_libraries(libraries);
    argument = home_argument(base, home->installs, count, libraries);
    argument.host_id = host_id;
    home->home = lay_out_argument(&argument);
    CHECK(home->home.status == SPISOR_OK);
    home->places.size = (uint32_t)sizeof home->places;
    resolve.environment = home->home.answer.variables;
    resolve.environment_count = home->home.answer.variable_count;
    CHECK(places_service()->resolve(NULL, &resolve, &home->places, &refusal) == SPISOR_OK);
    places_service()->release_refusal(NULL, &refusal);
}

static void open_session(session_home *home, const char *loader) {
    runtime_ui_session_argument argument = SPISOR_INIT(runtime_ui_session_argument);
    ui()->release_session_answer(NULL, &home->answer);
    ui()->release_refusal(NULL, &home->refusal);
    argument.places = &home->places;
    argument.loader = loader;
    home->status = ui()->session(NULL, &argument, &home->answer, &home->refusal);
}

static void finish_session_home(session_home *home) {
    ui()->release_session_answer(NULL, &home->answer);
    ui()->release_refusal(NULL, &home->refusal);
    places_service()->release_answer(NULL, &home->places);
    testing()->release_home_answer(NULL, &home->home.answer);
    testing()->release_refusal(NULL, &home->home.refusal);
    for (size_t index = 0; index < 3; index++) {
        spisor_value_release(&home->manifests[index]);
    }
    finish_parent();
}

static const char *value_text(const spisor_value *record, const char *key) {
    const spisor_value *member = record == NULL ? NULL : spisor_value_member(record, key, strlen(key));
    return member != NULL && member->kind == SPISOR_VALUE_TEXT ? member->text : NULL;
}

static const char *row_id(const spisor_value *group, size_t index) {
    return group != NULL && index < group->count ? value_text(&group->items[index], "id") : NULL;
}

static const runtime_setting_declaration *declaration_of(const runtime_config_plugin *config, const char *key) {
    for (size_t index = 0; index < config->setting_count; index++) {
        if (strcmp(config->settings[index].key, key) == 0) {
            return &config->settings[index];
        }
    }
    return NULL;
}

/** What config's get answers for key through the plugin's config, as text: a number printed, NULL for none. */
static void expect_config_value(const runtime_config_plugin *config, const char *key, const char *expected) {
    const runtime_config_definition *service = (const runtime_config_definition *)runtime_config_definition_of();
    runtime_config_get_argument argument = SPISOR_INIT(runtime_config_get_argument);
    spisor_value_tree value = SPISOR_INIT(spisor_value_tree);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    char printed[64] = "";
    argument.key = key;
    CHECK(service->get(NULL, config, &argument, &value, &refusal) == SPISOR_OK);
    if (value.root.kind == SPISOR_VALUE_TEXT) {
        snprintf(printed, sizeof printed, "%s", value.root.text);
    } else if (value.root.kind == SPISOR_VALUE_NUMBER) {
        snprintf(printed, sizeof printed, "%g", value.root.number);
    }
    check_text(printed, expected, key);
    service->release_tree(NULL, &value);
    service->release_refusal(NULL, &refusal);
}

static void expect_alpha_config(const session_home *home, const runtime_ui_session_plugin *alpha) {
    const runtime_config_plugin *config = alpha->config;
    const runtime_setting_declaration *depth = declaration_of(config, "depth");
    const runtime_setting_declaration *token = declaration_of(config, "token");
    check_text(config->plugin, "alpha", "the config's plugin");
    check_text(config->home, home->home.answer.home, "the config's home");
    check_text(config->portable, home->places.portable, "the config's portable place");
    CHECK(config->names == NULL && config->config != NULL && config->setting_count == 2);
    CHECK(depth != NULL && depth->layer == RUNTIME_SETTINGS_LAYER_APPLICATION);
    check_text(depth == NULL ? NULL : depth->type, "number", "depth's type");
    CHECK(token != NULL && token->layer == RUNTIME_SETTINGS_LAYER_DEFAULT);
    check_text(token == NULL ? NULL : token->type, "secret", "token's type");
    CHECK(config->places != NULL && config->places->force_file_store == home->places.force_file_store);
    check_text(config->places == NULL ? NULL : config->places->secrets_file, home->places.secrets_file,
               "the secrets file");
    expect_config_value(config, "greeting", "hi");
    expect_config_value(config, "depth", "2");
}

static void a_loader_answers_each_enabled_plugin_with_its_description_and_config(void) {
    static const char *const manifests[2] = {ALPHA, BETA};
    session_home home;
    spisor_value_tree settings = tree_of(parsed("{\"greeting\": \"hi\"}"));
    lay_out_session_home(&home, "two-plugins", LOADER, manifests, 2, &settings.root);
    char *before = tree_snapshot(parent, NULL);
    open_session(&home, LOADER);
    char *after = tree_snapshot(parent, NULL);
    CHECK(home.status == SPISOR_OK);
    check_text(after, before, "the home after a session");
    check_text(home.answer.host, LOADER, "the session's host");
    CHECK(home.answer.plugin_count == 2 && home.answer.descriptions != NULL &&
          home.answer.descriptions->count == 2);
    if (home.status == SPISOR_OK && home.answer.plugin_count == 2) {
        check_text(home.answer.plugins[0].id, "alpha", "the first plugin");
        check_text(home.answer.plugins[0].version, "1.0.0", "alpha's version");
        check_text(home.answer.plugins[1].id, "beta", "the second plugin");
        CHECK(home.answer.plugins[1].description == &home.answer.descriptions->items[1]);
        check_text(value_text(home.answer.plugins[0].description, "entry"), "index.js", "alpha's description");
        expect_alpha_config(&home, &home.answer.plugins[0]);
        check_text(home.answer.plugins[1].config->plugin, "beta", "beta's config");
        CHECK(home.answer.plugins[1].config->setting_count == 0);
    }
    CHECK(home.answer.ui != NULL && home.answer.ui->kind == SPISOR_VALUE_RECORD && home.answer.ui->count == 0);
    CHECK(home.answer.installed != NULL && home.answer.installed->count == 2);
    check_text(row_id(home.answer.installed, 0), "alpha", "the first installed row");
    CHECK(home.answer.detected != NULL && home.answer.detected->count == 0);
    CHECK(home.answer.available != NULL && home.answer.available->count == 0);
    CHECK(home.answer.problems != NULL && home.answer.problems->count == 0);
    CHECK(home.answer.source_count == 1 && home.answer.sources[0].kind == RUNTIME_CATALOG_LOCAL);
    spisor_memory_release(before);
    spisor_memory_release(after);
    spisor_value_release(&settings);
    finish_session_home(&home);
}

static void no_loader_answers_spisor_hosts_own_layer(void) {
    static const char *const manifests[1] = {GAMMA};
    session_home home;
    cJSON *refusal = NULL;
    cJSON *ui_settings = parsed("{\"theme\": \"dark\"}");
    lay_out_session_home(&home, "own-layer", "spisor-host", manifests, 1, NULL);
    CHECK(runtime_set_ecosystem_setting(home.home.answer.home, home.places.portable, "ui", ui_settings,
                                        RUNTIME_SETTINGS_LAYER_APPLICATION, &refusal));
    open_session(&home, NULL);
    CHECK(home.status == SPISOR_OK && home.answer.plugin_count == 1);
    check_text(home.answer.host, "spisor-host", "the session's host");
    check_text(home.answer.plugin_count == 1 ? home.answer.plugins[0].id : NULL, "gamma", "spisor-host's plugin");
    check_text(value_text(home.answer.ui, "theme"), "dark", "the host's ui settings");
    open_session(&home, LOADER);
    CHECK(home.status == SPISOR_REFUSED && strstr(home.refusal.detail, LOADER) != NULL);
    cJSON_Delete(ui_settings);
    cJSON_Delete(refusal);
    finish_session_home(&home);
}

static void no_loader_refuses_while_spisor_host_is_not_registered(void) {
    static const char *const manifests[1] = {ALPHA};
    session_home home;
    lay_out_session_home(&home, "unregistered", LOADER, manifests, 1, NULL);
    open_session(&home, NULL);
    CHECK(home.status == SPISOR_REFUSED && strstr(home.refusal.detail, "spisor-host") != NULL);
    finish_session_home(&home);
}

/** A catalog item for plugin id at version written in language, its one file a placeholder the session never fetches. */
static void list_plugin_item_in(cJSON *items, const char *id, const char *version, const char *language) {
    char item[512];
    snprintf(item, sizeof item, "{\"kind\": \"plugin\", \"language\": \"%s\", \"latest\": \"%s\", \"versions\": {\"%s\": {\"files\": %s}}}",
             language, version, version, FILE_REFERENCE);
    cJSON_AddItemToObject(items, id, parsed(item));
}

static void list_plugin_item(cJSON *items, const char *id, const char *version) {
    list_plugin_item_in(items, id, version, "typescript");
}

/** Writes index as the catalog the home's one local source names. */
static void write_catalog_index(const cJSON *index) {
    char *printed = cJSON_PrintUnformatted(index);
    char *catalog = joined(base, "catalog/index.json");
    write_text(catalog, printed);
    spisor_memory_release(catalog);
    cJSON_free(printed);
}

/** Rewrites the installed manifest of the plugin at `install` with `from` replaced by `to`, of the same length. */
static char *rewrite_manifest(const session_home *home, size_t install, const char *from, const char *to) {
    char *manifest = joined(home->home.answer.installs[install].bundle, "plugin.json");
    size_t length = 0;
    uint8_t *content = spisor_read_file_reporting(manifest, &length, &(spisor_read_outcome){SPISOR_READ_FAILED});
    char *text = content == NULL ? NULL : spisor_memory_allocate_zeroed(length + 1, 1);
    char *at;
    CHECK(text != NULL && strlen(from) == strlen(to));
    if (text != NULL) {
        memcpy(text, content, length);
    }
    at = text == NULL ? NULL : strstr(text, from);
    CHECK(at != NULL);
    if (at != NULL) {
        memcpy(at, to, strlen(to));
        CHECK(spisor_replace_file(manifest, (const uint8_t *)text, length) == SPISOR_OK);
    }
    spisor_memory_release(text);
    spisor_memory_release(content);
    return manifest;
}

/** The one problem the session answered, checked to name id at version (NULL for none) and to hold `detail`. */
static void expect_one_problem(const session_home *home, const char *id, const char *version, const char *detail) {
    const spisor_value *problems = home->answer.problems;
    const spisor_value *problem = problems != NULL && problems->count == 1 ? &problems->items[0] : NULL;
    const char *problem_detail = value_text(problem, "detail");
    CHECK(home->status == SPISOR_OK && problem != NULL);
    check_text(value_text(problem, "id"), id, "the problem's plugin");
    if (version == NULL) {
        CHECK(value_text(problem, "version") == NULL);
    } else {
        check_text(value_text(problem, "version"), version, "the problem's version");
    }
    CHECK(problem_detail != NULL && strstr(problem_detail, detail) != NULL);
    CHECK(value_text(problem, "fix") != NULL);
}

static void one_broken_plugin_of_two_answers_the_other_and_a_problem_naming_its_manifest(void) {
    static const char *const manifests[2] = {ALPHA, BETA};
    session_home home;
    cJSON *index = parsed("{\"format\": 2, \"items\": {}}");
    lay_out_session_home(&home, "one-broken", LOADER, manifests, 2, NULL);
    char *manifest = rewrite_manifest(&home, 0, "{\"id\"", "x\"id\"");
    list_plugin_item(cJSON_GetObjectItemCaseSensitive(index, "items"), "alpha", "1.0.0");
    list_plugin_item(cJSON_GetObjectItemCaseSensitive(index, "items"), "beta", "2.0.0");
    write_catalog_index(index);
    open_session(&home, LOADER);
    CHECK(home.status == SPISOR_OK && home.answer.plugin_count == 1);
    check_text(home.answer.plugin_count == 1 ? home.answer.plugins[0].id : NULL, "beta", "the plugin left");
    CHECK(home.answer.descriptions != NULL && home.answer.descriptions->count == 1);
    expect_one_problem(&home, "alpha", "1.0.0", home.home.answer.installs[0].bundle);
    CHECK(home.answer.installed != NULL && home.answer.installed->count == 1);
    check_text(row_id(home.answer.installed, 0), "beta", "the installed row");
    CHECK(home.answer.detected != NULL && home.answer.detected->count == 0);
    CHECK(home.answer.available != NULL && home.answer.available->count == 0);
    cJSON_Delete(index);
    spisor_memory_release(manifest);
    finish_session_home(&home);
}

static void an_unreadable_manifest_is_a_problem_saying_it_cannot_be_read(void) {
    static const char *const manifests[1] = {ALPHA};
    session_home home;
    lay_out_session_home(&home, "unreadable", LOADER, manifests, 1, NULL);
    char *manifest = rewrite_manifest(&home, 0, "{\"id\"", "x\"id\"");
    open_session(&home, LOADER);
    CHECK(home.status == SPISOR_OK && home.answer.plugin_count == 0);
    expect_one_problem(&home, "alpha", "1.0.0", "cannot be read");
    expect_one_problem(&home, "alpha", "1.0.0", home.home.answer.installs[0].bundle);
    spisor_memory_release(manifest);
    finish_session_home(&home);
}

static void a_manifest_its_scope_refuses_is_a_problem_naming_it(void) {
    static const char *const manifests[1] = {ALPHA};
    session_home home;
    lay_out_session_home(&home, "scope-refused", LOADER, manifests, 1, NULL);
    char *manifest = rewrite_manifest(&home, 0, "\"application\"", "\"applicatio_\"");
    open_session(&home, LOADER);
    CHECK(home.status == SPISOR_OK && home.answer.plugin_count == 0);
    expect_one_problem(&home, "alpha", "1.0.0", manifest);
    expect_one_problem(&home, "alpha", "1.0.0", "applicatio_");
    spisor_memory_release(manifest);
    finish_session_home(&home);
}

static void an_enabled_plugin_with_no_version_installed_is_a_problem_not_available(void) {
    static const char *const manifests[1] = {ALPHA};
    session_home home;
    cJSON *refusal = NULL;
    cJSON *enabled = cJSON_CreateTrue();
    cJSON *index = parsed("{\"format\": 2, \"items\": {}}");
    lay_out_session_home(&home, "not-installed", LOADER, manifests, 1, NULL);
    CHECK(runtime_set_ecosystem_setting(home.home.answer.home, home.places.portable, "plugins.omega.enabled", enabled,
                                        RUNTIME_SETTINGS_LAYER_APPLICATION, &refusal));
    list_plugin_item(cJSON_GetObjectItemCaseSensitive(index, "items"), "omega", "0.1.0");
    write_catalog_index(index);
    open_session(&home, LOADER);
    CHECK(home.status == SPISOR_OK && home.answer.plugin_count == 1);
    expect_one_problem(&home, "omega", "0.1.0", "enabled but not installed");
    CHECK(home.answer.available != NULL && home.answer.available->count == 0);
    CHECK(home.answer.detected != NULL && home.answer.detected->count == 0);
    cJSON_Delete(index);
    cJSON_Delete(enabled);
    cJSON_Delete(refusal);
    finish_session_home(&home);
}

static void sources_that_yield_no_catalog_are_one_problem_and_the_plugins_still_answer(void) {
    static const char *const manifests[1] = {ALPHA};
    session_home home;
    lay_out_session_home(&home, "no-catalog", LOADER, manifests, 1, NULL);
    char *catalog = joined(base, "catalog/index.json");
    CHECK(remove(catalog) == 0);
    open_session(&home, LOADER);
    CHECK(home.status == SPISOR_OK && home.answer.plugin_count == 1);
    expect_one_problem(&home, "catalog", NULL, "no catalog source yields a catalog");
    CHECK(home.answer.source_count == 1);
    spisor_memory_release(catalog);
    finish_session_home(&home);
}

static void the_groups_split_the_catalog_by_what_runs_and_what_is_cached(void) {
    static const char *const manifests[2] = {ALPHA, BETA};
    session_home home;
    cJSON *refusal = NULL;
    cJSON *disabled = cJSON_CreateFalse();
    cJSON *index = parsed("{\"format\": 2, \"items\": {}}");
    cJSON *items = cJSON_GetObjectItemCaseSensitive(index, "items");
    lay_out_session_home(&home, "groups", LOADER, manifests, 2, NULL);
    CHECK(runtime_set_ecosystem_setting(home.home.answer.home, home.places.portable, "plugins.beta.enabled", disabled,
                                        RUNTIME_SETTINGS_LAYER_APPLICATION, &refusal));
    list_plugin_item(items, "alpha", "1.0.0");
    list_plugin_item(items, "beta", "2.1.0");
    list_plugin_item(items, "omega", "0.1.0");
    write_catalog_index(index);
    open_session(&home, LOADER);
    CHECK(home.status == SPISOR_OK && home.answer.plugin_count == 1);
    CHECK(home.answer.installed->count == 1 && home.answer.detected->count == 1 && home.answer.available->count == 1);
    check_text(row_id(home.answer.installed, 0), "alpha", "the installed row");
    check_text(row_id(home.answer.detected, 0), "beta", "the detected row");
    check_text(value_text(&home.answer.detected->items[0], "version"), "2.0.0", "the detected row's cached version");
    check_text(row_id(home.answer.available, 0), "omega", "the available row");
    check_text(value_text(&home.answer.available->items[0], "version"), "0.1.0", "the available row's latest");
    CHECK(home.answer.problems != NULL && home.answer.problems->count == 0);
    cJSON_Delete(index);
    cJSON_Delete(disabled);
    cJSON_Delete(refusal);
    finish_session_home(&home);
}

static void expect_both_plugins_answered(const session_home *home, const char *label) {
    if (home->status != SPISOR_OK || home->answer.plugin_count != 2) {
        fprintf(stderr, "%s: expected both plugins, got status %d, %zu plugins and \"%s\"\n", label, (int)home->status,
                home->answer.plugin_count, home->refusal.detail == NULL ? "(none)" : home->refusal.detail);
        check_failures++;
        return;
    }
    check_text(home->answer.plugins[0].id, "alpha", label);
    check_text(home->answer.plugins[1].id, "beta", label);
}

/** No language can hold this loader's front door, which refuses its start, and the interface still lists its plugins. */
static void a_loader_requiring_no_plugin_over_two_languages_answers_its_plugins(void) {
    static const char *const manifests[2] = {ALPHA, BETA};
    session_home home;
    cJSON *index = parsed("{\"format\": 2, \"items\": {}}");
    cJSON *items = cJSON_GetObjectItemCaseSensitive(index, "items");
    lay_out_session_home(&home, "two-doors", LOADER, manifests, 2, NULL);
    list_plugin_item_in(items, "alpha", "1.0.0", "typescript");
    list_plugin_item_in(items, "beta", "2.0.0", "fixture-one");
    write_catalog_index(index);
    open_session(&home, LOADER);
    expect_both_plugins_answered(&home, "two languages and no front door");
    cJSON_Delete(index);
    finish_session_home(&home);
}

/** A first required plugin no catalog lists refuses the loader's start, and the interface still lists its plugins. */
static void a_loader_whose_first_required_plugin_no_catalog_lists_answers_its_plugins(void) {
    static const char *const manifests[2] = {ALPHA, BETA};
    session_home home;
    lay_out_session_home(&home, "unlisted-door", LOADER, manifests, 2, NULL);
    char *standard = joined(home.places.cache, "bundles/" LOADER "/1.0.0/host.json");
    char *directory = joined(home.places.cache, "bundles/" LOADER "/1.0.0");
    CHECK(spisor_make_directories(directory) == SPISOR_OK);
    write_text(standard, "{\"plugins\": [\"brewed\"]}");
    open_session(&home, LOADER);
    expect_both_plugins_answered(&home, "an unlisted front door");
    spisor_memory_release(directory);
    spisor_memory_release(standard);
    finish_session_home(&home);
}

static void answers_malformed_for_places_that_are_not_absolute_and_a_loader_that_is_no_host_id(void) {
    static const char *const manifests[1] = {ALPHA};
    session_home home;
    const char *state;
    lay_out_session_home(&home, "malformed", LOADER, manifests, 1, NULL);
    open_session(&home, "no/host");
    CHECK(home.status == SPISOR_MALFORMED);
    state = home.places.state;
    home.places.state = "relative/state";
    open_session(&home, LOADER);
    CHECK(home.status == SPISOR_MALFORMED);
    home.places.state = state;
    finish_session_home(&home);
}

int main(void) {
    a_loader_answers_each_enabled_plugin_with_its_description_and_config();
    no_loader_answers_spisor_hosts_own_layer();
    no_loader_refuses_while_spisor_host_is_not_registered();
    one_broken_plugin_of_two_answers_the_other_and_a_problem_naming_its_manifest();
    an_unreadable_manifest_is_a_problem_saying_it_cannot_be_read();
    a_manifest_its_scope_refuses_is_a_problem_naming_it();
    an_enabled_plugin_with_no_version_installed_is_a_problem_not_available();
    sources_that_yield_no_catalog_are_one_problem_and_the_plugins_still_answer();
    the_groups_split_the_catalog_by_what_runs_and_what_is_cached();
    a_loader_requiring_no_plugin_over_two_languages_answers_its_plugins();
    a_loader_whose_first_required_plugin_no_catalog_lists_answers_its_plugins();
    answers_malformed_for_places_that_are_not_absolute_and_a_loader_that_is_no_host_id();
    return check_summary("ui_session_test");
}
