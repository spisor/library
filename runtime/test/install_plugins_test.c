#include "install_fixture.h"
#include "spisor/allocation.h"

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#include "boot_hint.h"
#include "runtime/install.h"
#include "runtime/ui.h"
#include "tree_snapshot.h"

#define CLAUDE "claude-loader"
#define CODEX "codex-loader"

/** What one install.plugins call answered, its answer as {installed, cached, unavailable, enabled, javascript}. */
typedef struct plugins_result {
    spisor_status status;
    char detail[2048];
    cJSON *answer;
} plugins_result;

static void place_binary(const install_base *base, const char *name) {
#if defined(_WIN32)
    char file[128];
    snprintf(file, sizeof file, "%s.cmd", name);
    write_in(base->path, file, "@echo off\r\n");
#else
    char *path = joined(base->path, name);
    write_file(path, "#!/bin/sh\n");
    CHECK(chmod(path, 0755) == 0);
    spisor_memory_release(path);
#endif
}

static void make_application_home(const install_base *base, const char *application) {
    char rest[128];
    snprintf(rest, sizeof rest, "homes/%s", application);
    char *home = joined(base->user, rest);
    CHECK(spisor_make_directories(home) == SPISOR_OK);
    spisor_memory_release(home);
}

/**
 * An installed ecosystem: the foundation, announcer enabled for every host, the plugins wanted (fixture-one, whose
 * host announcer brings) and other (fixture-two, whose host nothing installs yet) only listed, and two loaders whose
 * applications are on the PATH.
 */
static void prepare_world(install_base *base, const char *label) {
    start_install_base(base, "install-plugins", label);
    list_foundation(base);
    list_plugin(base, "announcer", "1.0.0", "fixture-one");
    list_plugin(base, "wanted", "1.0.0", "fixture-one");
    list_plugin(base, "other", "2.0.0", "fixture-two");
    list_language_host(base, "fixture-two", "native", "1.0.0", "{\"entry\": \"program\", \"runtime\": \"native\"}");
    list_loader(base, CLAUDE, "1.0.0", application_json("claude", "claude-binary", "{}"), "[]", 0, "[]");
    list_loader(base, CODEX, "1.0.0", application_json("codex", "codex-binary", "{}"), "[]", 0, "[]");
    write_catalog(base);
    place_binary(base, "claude-binary");
    place_binary(base, "codex-binary");
    make_application_home(base, "claude");
    make_application_home(base, "codex");
    cJSON *report = expect_report(install_with(base, "{\"defaults\": [\"announcer\"]}", NULL), "the ecosystem");
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(report, "loaders")) == 2);
    cJSON_Delete(report);
}

static cJSON *texts_json(const char *const *texts, size_t count) {
    cJSON *array = cJSON_CreateArray();
    for (size_t index = 0; index < count; index++) {
        cJSON_AddItemToArray(array, cJSON_CreateString(texts[index]));
    }
    return array;
}

static cJSON *answer_json(const runtime_install_plugins_answer *answer) {
    cJSON *json = cJSON_CreateObject();
    cJSON *installed = cJSON_AddArrayToObject(json, "installed");
    cJSON *unavailable = cJSON_AddArrayToObject(json, "unavailable");
    for (size_t index = 0; index < answer->installed_count; index++) {
        cJSON_AddItemToArray(installed, cJSON_CreateString(answer->installed[index].id));
    }
    for (size_t index = 0; index < answer->unavailable_count; index++) {
        cJSON_AddItemToArray(unavailable, cJSON_CreateString(answer->unavailable[index].id));
    }
    cJSON_AddItemToObject(json, "cached", texts_json(answer->cached, answer->cached_count));
    cJSON_AddItemToObject(json, "enabled", texts_json(answer->enabled, answer->enabled_count));
    cJSON_AddBoolToObject(json, "javascript", answer->javascript != NULL);
    return json;
}

static plugins_result install_plugins_for(const install_base *base, const char *host, const char *const *plugins,
                                          size_t count) {
    const runtime_install_definition *install = runtime_install_definition_of();
    runtime_variable environment[2] = {{"PATH", base->path}, {"HOME", base->user}};
    runtime_install_plugins_argument argument = SPISOR_INIT(runtime_install_plugins_argument);
    runtime_install_plugins_answer answer;
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    plugins_result result;
    memset(&answer, 0, sizeof answer);
    answer.size = (uint32_t)sizeof answer;
    argument.portable = base->portable;
    argument.state = base->state;
    argument.cache = base->cache;
    argument.environment = environment;
    argument.environment_count = 2;
    argument.host = host;
    argument.plugins = plugins;
    argument.plugin_count = count;
    result.status = install->plugins(install->context, &argument, &answer, &refusal);
    snprintf(result.detail, sizeof result.detail, "%s", refusal.detail == NULL ? "" : refusal.detail);
    result.answer = result.status == SPISOR_OK ? answer_json(&answer) : NULL;
    install->release_plugins_answer(install->context, &answer);
    install->release_refusal(install->context, &refusal);
    return result;
}

static plugins_result install_one(const install_base *base, const char *host, const char *plugin) {
    const char *plugins[1] = {plugin};
    return install_plugins_for(base, host, plugins, 1);
}

static void expect_answered(const plugins_result *result, const char *label) {
    if (result->status != SPISOR_OK) {
        fprintf(stderr, "%s: answered %d \"%s\"\n", label, (int)result->status, result->detail);
        check_failures++;
    }
}

/** The plugin ids ui's session answers for host (NULL for spisor-host), as one JSON array. */
static cJSON *session_plugins(const install_base *base, const char *host) {
    const runtime_ui_definition *ui = (const runtime_ui_definition *)runtime_ui_definition_of();
    char *secrets = joined(base->state, "secrets");
    runtime_places_answer places;
    memset(&places, 0, sizeof places);
    places.size = (uint32_t)sizeof places;
    places.home = base->root;
    places.portable = base->portable;
    places.state = base->state;
    places.cache = base->cache;
    places.secrets_file = secrets;
    runtime_ui_session_argument argument = SPISOR_INIT(runtime_ui_session_argument);
    runtime_ui_session_answer answer = SPISOR_INIT(runtime_ui_session_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    argument.places = &places;
    argument.loader = host;
    CHECK(ui->session(NULL, &argument, &answer, &refusal) == SPISOR_OK);
    cJSON *ids = cJSON_CreateArray();
    for (size_t index = 0; index < answer.plugin_count; index++) {
        cJSON_AddItemToArray(ids, cJSON_CreateString(answer.plugins[index].id));
    }
    ui->release_session_answer(NULL, &answer);
    ui->release_refusal(NULL, &refusal);
    spisor_memory_release(secrets);
    return ids;
}

static void expect_session_plugins(const install_base *base, const char *host, const char *expected_json,
                                   const char *label) {
    cJSON *ids = session_plugins(base, host);
    expect_json(ids, expected_json, label);
    cJSON_Delete(ids);
}

/** The application layer of the settings of the host whose home is `home`, read alone. */
static cJSON *application_layer(const char *home) {
    cJSON *refusal = NULL;
    cJSON *values = runtime_ecosystem_settings_values(home, NULL, &refusal);
    CHECK(values != NULL && refusal == NULL);
    cJSON_Delete(refusal);
    return values;
}

static void expect_enabled_in(const char *home, const char *plugin, int enabled, const char *label) {
    cJSON *values = application_layer(home);
    char slashed[128];
    snprintf(slashed, sizeof slashed, "plugins/%s/enabled", plugin);
    if (cJSON_IsTrue(value_at(values, slashed)) != enabled) {
        fprintf(stderr, "%s: %s is%s enabled in %s\n", label, plugin, enabled ? " not" : "", home);
        check_failures++;
    }
    cJSON_Delete(values);
}

static char *loader_home(const install_base *base, const char *application) {
    char rest[128];
    snprintf(rest, sizeof rest, "homes/%s", application);
    return joined(base->user, rest);
}

/** Every file and folder a host's settings and records hold, outside the cache: what install.plugins may not touch. */
static char *host_snapshot(const install_base *base, const char *host, const char *home) {
    char registration[256];
    char references[256];
    snprintf(registration, sizeof registration, "hosts/%s.json", host);
    snprintf(references, sizeof references, "bundles/%s.json", host);
    char *home_tree = tree_snapshot(home, NULL);
    char *registration_text = text_in(base->state, registration);
    char *references_text = text_in(base->state, references);
    const char *parts[3] = {home_tree, registration_text == NULL ? "(none)" : registration_text,
                            references_text == NULL ? "(none)" : references_text};
    size_t length = strlen(parts[0]) + strlen(parts[1]) + strlen(parts[2]) + 3;
    char *snapshot = spisor_memory_allocate(length);
    snprintf(snapshot, length, "%s\n%s\n%s", parts[0], parts[1], parts[2]);
    spisor_memory_release(references_text);
    spisor_memory_release(registration_text);
    spisor_memory_release(home_tree);
    return snapshot;
}

static void expect_same_text(char *before, char *after, const char *label) {
    if (strcmp(before, after) != 0) {
        fprintf(stderr, "%s changed\n", label);
        check_failures++;
    }
    spisor_memory_release(before);
    spisor_memory_release(after);
}

static void a_rows_plugin_becomes_enabled_for_the_loader_and_its_next_session_lists_it(void) {
    install_base base;
    prepare_world(&base, "row");
    char *claude_home = loader_home(&base, "claude");
    char *codex_home = loader_home(&base, "codex");
    char *own_home = joined(base.state, "home/spisor-host");
    char *codex_before = host_snapshot(&base, CODEX, codex_home);
    char *own_before = host_snapshot(&base, "spisor-host", own_home);
    char *portable_before = tree_snapshot(base.portable, NULL);
    expect_session_plugins(&base, CLAUDE, "[\"announcer\"]", "claude's session before");
    plugins_result result = install_one(&base, CLAUDE, "wanted");
    expect_answered(&result, "a row's install");
    expect_json(result.answer,
                "{\"installed\": [\"wanted\"], \"unavailable\": [], \"cached\": [], \"enabled\": [\"wanted\"], "
                "\"javascript\": false}",
                "a row's install answer");
    expect_enabled_in(claude_home, "wanted", 1, "claude's settings");
    expect_session_plugins(&base, CLAUDE, "[\"announcer\", \"wanted\"]", "claude's next session");
    expect_session_plugins(&base, CODEX, "[\"announcer\"]", "codex's session");
    expect_session_plugins(&base, NULL, "[\"announcer\"]", "spisor-host's session");
    cJSON *references = json_in(base.state, "bundles/" CLAUDE ".json");
    expect_json(value_at(references, "run/wanted"), "\"1.0.0\"", "claude's reference to wanted");
    expect_same_text(codex_before, host_snapshot(&base, CODEX, codex_home), "codex-loader");
    expect_same_text(own_before, host_snapshot(&base, "spisor-host", own_home), "spisor-host's settings");
    expect_same_text(portable_before, tree_snapshot(base.portable, NULL), "the portable place");
    cJSON_Delete(references);
    cJSON_Delete(result.answer);
    spisor_memory_release(own_home);
    spisor_memory_release(codex_home);
    spisor_memory_release(claude_home);
    finish_install_base(&base);
}

static void no_host_installs_for_spisor_host_alone(void) {
    install_base base;
    prepare_world(&base, "own");
    char *claude_home = loader_home(&base, "claude");
    char *own_home = joined(base.state, "home/spisor-host");
    char *claude_before = host_snapshot(&base, CLAUDE, claude_home);
    plugins_result result = install_one(&base, NULL, "wanted");
    expect_answered(&result, "spisor-host's install");
    expect_enabled_in(own_home, "wanted", 1, "spisor-host's settings");
    expect_session_plugins(&base, NULL, "[\"announcer\", \"wanted\"]", "spisor-host's next session");
    expect_session_plugins(&base, CLAUDE, "[\"announcer\"]", "claude's session");
    expect_same_text(claude_before, host_snapshot(&base, CLAUDE, claude_home), "claude-loader");
    cJSON_Delete(result.answer);
    spisor_memory_release(own_home);
    spisor_memory_release(claude_home);
    finish_install_base(&base);
}

static void installs_and_hints_the_language_host_a_plugin_needs_only_while_it_has_none(void) {
    install_base base;
    prepare_world(&base, "language-host");
    CHECK(!exists_in(base.cache, "hosts/fixture-two-host.json"));
    plugins_result first = install_one(&base, CLAUDE, "other");
    expect_answered(&first, "the first install");
    expect_json(value_at(first.answer, "installed"), "[\"other\", \"fixture-two-host\"]", "the first install");
    expect_json(value_at(first.answer, "unavailable"), "[]", "the first install's unavailable");
    cJSON *hint = json_in(base.cache, "hosts/fixture-two-host.json");
    expect_json(value_at(hint, "host"), "\"1.0.0\"", "fixture-two-host's hinted version");
    expect_json(value_at(hint, "libraries/runtime/version"), "\"0.1.0\"", "the runtime fixture-two-host runs on");
    expect_json(value_at(hint, "libraries/forebay-library/version"), "\"0.1.2\"", "its forebay-library");
    cJSON *references = json_in(base.state, "bundles/spisor-host.json");
    expect_json(value_at(references, "run/fixture-two-host"), "\"1.0.0\"", "spisor-host's reference to the host");
    expect_session_plugins(&base, CLAUDE, "[\"announcer\", \"other\"]", "claude's next session");
    char *hint_before = text_in(base.cache, "hosts/fixture-two-host.json");
    plugins_result second = install_one(&base, CLAUDE, "other");
    expect_answered(&second, "the second install");
    expect_json(second.answer,
                "{\"installed\": [], \"unavailable\": [], \"cached\": [\"other@2.0.0\"], \"enabled\": [], "
                "\"javascript\": false}",
                "the second install answer");
    expect_same_text(hint_before, text_in(base.cache, "hosts/fixture-two-host.json"), "fixture-two-host's hint");
    cJSON_Delete(references);
    cJSON_Delete(hint);
    cJSON_Delete(second.answer);
    cJSON_Delete(first.answer);
    finish_install_base(&base);
}

static void expect_refused_having_written_nothing(const install_base *base, const char *host,
                                                  const char *const *plugins, size_t count, const char *named,
                                                  const char *label) {
    size_t before_length = 0;
    size_t after_length = 0;
    char *before = tree_snapshot_sized(base->root, NULL, &before_length);
    plugins_result result = install_plugins_for(base, host, plugins, count);
    char *after = tree_snapshot_sized(base->root, NULL, &after_length);
    if (result.status != SPISOR_REFUSED || strstr(result.detail, named) == NULL) {
        fprintf(stderr, "%s: answered %d \"%s\", not a refusal naming %s\n", label, (int)result.status, result.detail,
                named);
        check_failures++;
    }
    if (before_length != after_length || memcmp(before, after, before_length) != 0) {
        fprintf(stderr, "%s: the places changed\n", label);
        check_failures++;
    }
    cJSON_Delete(result.answer);
    spisor_memory_release(after);
    spisor_memory_release(before);
}

static void refuses_a_plugin_no_catalog_lists_having_written_nothing(void) {
    static const char *const unlisted[2] = {"wanted", "ghost"};
    static const char *const no_plugin[1] = {"forebay-library"};
    install_base base;
    prepare_world(&base, "unlisted");
    expect_refused_having_written_nothing(&base, CLAUDE, unlisted, 2, "ghost", "an unlisted plugin");
    expect_refused_having_written_nothing(&base, CLAUDE, no_plugin, 1, "forebay-library", "an item that is no plugin");
    finish_install_base(&base);
}

static void refuses_a_plugin_whose_language_no_catalog_hosts_having_written_nothing(void) {
    static const char *const unhosted[1] = {"stranger"};
    install_base base;
    prepare_world(&base, "unhosted");
    list_plugin(&base, "stranger", "1.0.0", "fixture-three");
    write_catalog(&base);
    expect_refused_having_written_nothing(&base, CLAUDE, unhosted, 1, "fixture-three", "an unhosted language");
    finish_install_base(&base);
}

static void refuses_a_host_that_is_not_registered_having_written_nothing(void) {
    static const char *const wanted[1] = {"wanted"};
    install_base base;
    prepare_world(&base, "unregistered");
    expect_refused_having_written_nothing(&base, "ghost-loader", wanted, 1, "ghost-loader", "an unregistered host");
    finish_install_base(&base);
}

static void answers_malformed_for_no_plugins_a_host_that_is_no_host_id_or_a_relative_place(void) {
    static const char *const wanted[1] = {"wanted"};
    static const char *const blank[1] = {NULL};
    install_base base;
    prepare_world(&base, "malformed");
    CHECK(install_plugins_for(&base, CLAUDE, wanted, 0).status == SPISOR_MALFORMED);
    CHECK(install_plugins_for(&base, CLAUDE, NULL, 1).status == SPISOR_MALFORMED);
    CHECK(install_plugins_for(&base, CLAUDE, blank, 1).status == SPISOR_MALFORMED);
    CHECK(install_plugins_for(&base, "no/host", wanted, 1).status == SPISOR_MALFORMED);
    char relative[] = "relative/state";
    char *state = base.state;
    base.state = relative;
    CHECK(install_plugins_for(&base, CLAUDE, wanted, 1).status == SPISOR_MALFORMED);
    base.state = state;
    finish_install_base(&base);
}

int main(void) {
    user_path_guard guard;
    guard_user_path(&guard);
    a_rows_plugin_becomes_enabled_for_the_loader_and_its_next_session_lists_it();
    no_host_installs_for_spisor_host_alone();
    installs_and_hints_the_language_host_a_plugin_needs_only_while_it_has_none();
    refuses_a_plugin_no_catalog_lists_having_written_nothing();
    refuses_a_plugin_whose_language_no_catalog_hosts_having_written_nothing();
    refuses_a_host_that_is_not_registered_having_written_nothing();
    answers_malformed_for_no_plugins_a_host_that_is_no_host_id_or_a_relative_place();
    expect_user_path_unchanged(&guard);
    return check_summary("install_plugins_test");
}
