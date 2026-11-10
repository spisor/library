#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache.h"
#include "catalog_call.h"
#include "catalog_url.h"
#include "check.h"
#include "fetch_hooks.h"
#include "http_fixture.h"
#include "install_by_id.h"
#include "paths.h"
#include "service_scratch.h"
#include "sha256.h"
#include "tree_snapshot.h"

#define PLUGIN_MANIFEST "{\"importName\":\"greeter\",\"api\":1,\"version\":\"1.0.0\",\"entry\":\"dist/index.js\"}"
#define PLUGIN_ENTRY "export default {};"
#define REPOSITORY(identifier, name_text) \
    "{\"id\":" identifier ",\"name\":\"" name_text "\",\"full_name\":\"o/" name_text "\",\"default_branch\":\"main\"}"
#define ROUTE(path, status, body) {path, status, NULL, body, sizeof body - 1, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0}
#define EMPTY_ROUTE(path, status) {path, status, NULL, "", 0, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0}

static unsigned short server_port;
static char scratch[1024];
static const char *plugin_folder = NULL;
static char release_json_good[1600];
static char release_json_tampered[1600];
static const char release_json_without_index[] = "{\"assets\":[{\"name\":\"other.zip\",\"browser_download_url\":\"https://example.test/other.zip\"}]}";

static char *in_scratch(const char *relative) {
    return joined_path(scratch, relative);
}

static void write_in_scratch(const char *relative, const char *text) {
    char *path = in_scratch(relative);
    char *parent = joined_path(path, "..");
    CHECK(spisor_make_directories(parent) == 1);
    CHECK(spisor_replace_file(path, (const uint8_t *)text, strlen(text)) == 1);
    free(parent);
    free(path);
}

static void sha256_of(const char *text, char hex[RUNTIME_SHA256_HEX_SIZE]) {
    runtime_sha256 hash;
    runtime_sha256_start(&hash);
    runtime_sha256_add(&hash, (const uint8_t *)text, strlen(text));
    runtime_sha256_finish_hex(&hash, hex);
}

static void add_file_reference(cJSON *files, const char *key, const char *url_prefix, const char *file_name,
                               const char *text) {
    char digest[RUNTIME_SHA256_HEX_SIZE];
    sha256_of(text, digest);
    char url[1600];
    snprintf(url, sizeof url, "%s%s%s", url_prefix == NULL ? "" : url_prefix, url_prefix == NULL ? "" : "/", file_name);
    cJSON *reference = cJSON_AddObjectToObject(files, key);
    cJSON_AddStringToObject(reference, "url", url);
    cJSON_AddStringToObject(reference, "sha256", digest);
    cJSON_AddNumberToObject(reference, "size", (double)strlen(text));
}

/** The catalog item a release publishes; its file urls are file_name beside the index when url_prefix is NULL. */
static cJSON *release_item(const char *url_prefix, const char *entry_digest_text) {
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "kind", "plugin");
    cJSON_AddStringToObject(item, "latest", "1.0.0");
    cJSON *version = cJSON_AddObjectToObject(cJSON_AddObjectToObject(item, "versions"), "1.0.0");
    cJSON_AddStringToObject(version, "commit", "abc123");
    cJSON *files = cJSON_AddObjectToObject(version, "files");
    add_file_reference(files, "plugin.json", url_prefix, "plugin.json", PLUGIN_MANIFEST);
    add_file_reference(files, "dist/index.js", url_prefix, "index.js", entry_digest_text);
    return item;
}

static void write_release(const char *directory, const char *entry_digest_text) {
    char relative[128];
    snprintf(relative, sizeof relative, "%s/plugin.json", directory);
    write_in_scratch(relative, PLUGIN_MANIFEST);
    snprintf(relative, sizeof relative, "%s/index.js", directory);
    write_in_scratch(relative, PLUGIN_ENTRY);
    cJSON *item = release_item(NULL, entry_digest_text);
    char *index = cJSON_PrintUnformatted(item);
    snprintf(relative, sizeof relative, "%s/weivel-index.json", directory);
    write_in_scratch(relative, index);
    cJSON_free(index);
    cJSON_Delete(item);
}

static void write_release_json(char *json, size_t size, const char *directory) {
    char *index_path = in_scratch(directory);
    char *index_file = joined_path(index_path, "weivel-index.json");
    char *url = runtime_catalog_file_url_of_path(RUNTIME_HOST_IS_WINDOWS, index_file);
    CHECK(url != NULL);
    snprintf(json, size, "{\"assets\":[{\"name\":\"weivel-index.json\",\"browser_download_url\":\"%s\"}]}",
             url == NULL ? "" : url);
    free(url);
    free(index_file);
    free(index_path);
}

static void prepare_releases(void) {
    scratch_in(scratch, sizeof scratch, "install-by-id", "world");
    write_release("release", PLUGIN_ENTRY);
    write_release("release-tampered", "export default 1;;");
    write_release_json(release_json_good, sizeof release_json_good, "release");
    write_release_json(release_json_tampered, sizeof release_json_tampered, "release-tampered");
}

static char *cache_directory(void) {
    return in_scratch("cache");
}

/** Each id deploys into a folder of its own, since a plugin directory holds one plugin per import name. */
static char *plugin_directory(const char *id) {
    char folder[128];
    snprintf(folder, sizeof folder, "plugins-%s", id);
    for (char *at = folder; *at != '\0'; at++) {
        *at = *at == ':' || *at == '/' ? '-' : *at;
    }
    return in_scratch(plugin_folder != NULL ? plugin_folder : folder);
}

static catalog_answer install_with(const char *id, const char *extra_json) {
    char *cache = cache_directory();
    char *plugins = plugin_directory(id);
    char api_base[128];
    snprintf(api_base, sizeof api_base, "http://127.0.0.1:%u", (unsigned)server_port);
    cJSON *argument = extra_json == NULL ? cJSON_CreateObject() : parsed(extra_json);
    cJSON_AddStringToObject(argument, "id", id);
    cJSON_AddStringToObject(argument, "cache", cache);
    cJSON_AddStringToObject(argument, "pluginDir", plugins);
    cJSON_AddStringToObject(argument, "apiBase", api_base);
    free(plugins);
    free(cache);
    return call_catalog(runtime_install_by_id, argument);
}

static int succeeded(const catalog_answer answer, const char *label) {
    int matched = answer.status == SPISOR_STATUS_SUCCESS;
    if (!matched) {
        const char *message = text_at(answer.tree, "message");
        fprintf(stderr, "%s: expected success, got status %d and \"%s\"\n", label, (int)answer.status,
                message == NULL ? "(none)" : message);
        check_failures++;
    }
    return matched;
}

static int exists_in_scratch(const char *relative) {
    char *path = in_scratch(relative);
    int exists = spisor_file_exists(path);
    free(path);
    return exists;
}

static int ids_are(const catalog_answer answer, const char *expected) {
    const cJSON *ids = cJSON_GetObjectItemCaseSensitive(answer.tree, "ids");
    char *printed = cJSON_IsArray(ids) ? cJSON_PrintUnformatted(ids) : NULL;
    int matched = printed != NULL && strcmp(printed, expected) == 0;
    if (!matched) {
        fprintf(stderr, "expected ids %s, got %s\n", expected, printed == NULL ? "(none)" : printed);
        check_failures++;
    }
    cJSON_free(printed);
    return matched;
}

static int flag_is(const catalog_answer answer, const char *member, int expected) {
    const cJSON *flag = cJSON_GetObjectItemCaseSensitive(answer.tree, member);
    int matched = expected < 0 ? flag == NULL : cJSON_IsBool(flag) && cJSON_IsTrue(flag) == expected;
    if (!matched) {
        fprintf(stderr, "expected %s to be %d\n", member, expected);
        check_failures++;
    }
    return matched;
}

static void installs_from_the_release_index_and_checks_digests(void) {
    catalog_answer answer = install_with("github:5", NULL);
    CHECK(succeeded(answer, "call 1"));
    check_text(text_at(answer.tree, "id"), "github:5", "id");
    check_text(text_at(answer.tree, "name"), "greeter", "name");
    check_text(text_at(answer.tree, "version"), "1.0.0", "version");
    check_text(text_at(answer.tree, "commit"), "abc123", "commit");
    char *index_path = in_scratch("release/weivel-index.json");
    char index_digest[RUNTIME_SHA256_HEX_SIZE];
    CHECK(spisor_sha256_file(index_path, index_digest) == 1);
    check_text(text_at(answer.tree, "sha256"), index_digest, "digest of the index");
    free(index_path);
    CHECK(exists_in_scratch("cache/bundles/github+5/1.0.0/plugin.json"));
    CHECK(exists_in_scratch("cache/bundles/github+5/1.0.0/dist/index.js"));
    CHECK(exists_in_scratch("plugins-github-5/greeter.json"));
    CHECK(exists_in_scratch("plugins-github-5/greeter.js"));
    CHECK(exists_in_scratch("plugins-github-5/greeter_identity.json"));
    cJSON_Delete(answer.tree);
}

static void install_by_id_records_the_set_and_the_primary(void) {
    catalog_answer answer = install_with("github:5", NULL);
    CHECK(succeeded(answer, "call 2"));
    CHECK(ids_are(answer, "[\"github:5\",\"github:6\"]"));
    check_text(text_at(answer.tree, "primary"), "github:5", "primary");
    CHECK(flag_is(answer, "primaryConfirmed", 1));
    CHECK(flag_is(answer, "needsUserAcceptance", -1));
    cJSON_Delete(answer.tree);
}

static void an_id_key_lands_in_its_folder_on_windows_paths(void) {
    char *cache = cache_directory();
    char *directory_url = runtime_catalog_file_url_of_path(RUNTIME_HOST_IS_WINDOWS, scratch);
    char url_prefix[1600];
    snprintf(url_prefix, sizeof url_prefix, "%s/release", directory_url);
    cJSON *item = release_item(url_prefix, PLUGIN_ENTRY);
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "cache", cache);
    cJSON_AddStringToObject(argument, "id", "github:5/kernel");
    cJSON_AddItemToObject(argument, "item", item);
    cJSON_AddStringToObject(argument, "version", "1.0.0");
    catalog_answer answer = call_catalog(runtime_cache_install, argument);
    CHECK(succeeded(answer, "call 3"));
    const char *directory = text_at(answer.tree, "directory");
    CHECK(directory != NULL && strncmp(directory, cache, strlen(cache)) == 0);
    CHECK(directory != NULL && strchr(directory + strlen(cache), ':') == NULL);
    CHECK(directory != NULL && strstr(directory, "github+5=kernel") != NULL);
    cJSON_Delete(answer.tree);
    free(directory_url);
    free(cache);
}

static void a_tampered_file_is_refused_and_nothing_is_deployed(void) {
    plugin_folder = "plugins-tampered";
    catalog_answer answer = install_with("github:10", NULL);
    CHECK(answer.status == SPISOR_STATUS_FAILURE);
    CHECK(text_at(answer.tree, "message") != NULL && strstr(text_at(answer.tree, "message"), "index.js") != NULL);
    CHECK(!exists_in_scratch("cache/bundles/github+10/1.0.0/.installed.json"));
    CHECK(!exists_in_scratch("plugins-tampered"));
    cJSON_Delete(answer.tree);
    plugin_folder = NULL;
}

static void a_name_taken_during_the_install_is_refused_and_nothing_is_installed(void) {
    plugin_folder = "plugins-taken";
    catalog_answer answer = install_with("github:9", NULL);
    CHECK(expect_refusal(answer, "when the install began", "taken name"));
    CHECK(!exists_in_scratch("cache/bundles/github+9"));
    CHECK(!exists_in_scratch("plugins-taken"));
    plugin_folder = NULL;
}

static void an_unreadable_start_file_installs_and_says_the_primary_is_unconfirmed(void) {
    catalog_answer answer = install_with("github:7", NULL);
    CHECK(succeeded(answer, "call 4"));
    CHECK(ids_are(answer, "[\"github:7\"]"));
    check_text(text_at(answer.tree, "primary"), "github:7", "primary is the id given");
    CHECK(flag_is(answer, "primaryConfirmed", 0));
    CHECK(exists_in_scratch("cache/bundles/github+7/1.0.0/plugin.json"));
    cJSON_Delete(answer.tree);
}

static char *read_scratch(const char *relative) {
    char *path = in_scratch(relative);
    size_t length = 0;
    char *text = (char *)spisor_read_file(path, &length);
    free(path);
    return text;
}

static int record_primary_is(const char *record, const char *primary) {
    cJSON *tree = record == NULL ? NULL : parsed(record);
    const char *found = text_at(tree, "primary");
    int matched = found != NULL && strcmp(found, primary) == 0;
    cJSON_Delete(tree);
    return matched;
}

static void the_first_install_records_the_set_and_the_primary(void) {
    catalog_answer answer = install_with("github:11", NULL);
    CHECK(succeeded(answer, "first install"));
    CHECK(flag_is(answer, "needsUserAcceptance", -1));
    check_text(text_at(answer.tree, "version"), "1.0.0", "version");
    CHECK(exists_in_scratch("cache/bundles/github+11/1.0.0/plugin.json"));
    char *record = read_scratch("cache/identities/github+11.json");
    cJSON *recorded = record == NULL ? NULL : parsed(record);
    check_text(text_at(recorded, "primary"), "github:11", "recorded primary");
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(recorded, "ids")) == 1);
    cJSON_Delete(recorded);
    free(record);
    cJSON_Delete(answer.tree);
}

static void an_unchanged_primary_installs_again_without_asking(void) {
    catalog_answer answer = install_with("github:11", NULL);
    CHECK(succeeded(answer, "second install"));
    CHECK(flag_is(answer, "needsUserAcceptance", -1));
    check_text(text_at(answer.tree, "version"), "1.0.0", "version");
    cJSON_Delete(answer.tree);
}

static void write_record(const char *folder, const char *primary) {
    char relative[128];
    snprintf(relative, sizeof relative, "cache/identities/%s.json", folder);
    char text[256];
    snprintf(text, sizeof text, "{\"ids\":[\"%s\"],\"primary\":\"%s\"}", primary, primary);
    write_in_scratch(relative, text);
}

static void a_changed_primary_is_refused_until_the_user_accepts_it(void) {
    write_record("github+12", "github:5");
    catalog_answer refused = install_with("github:12", NULL);
    CHECK(succeeded(refused, "changed primary"));
    CHECK(flag_is(refused, "needsUserAcceptance", 1));
    check_text(text_at(refused.tree, "recordedPrimary"), "github:5", "recorded");
    check_text(text_at(refused.tree, "primary"), "github:12", "new primary");
    CHECK(!cJSON_HasObjectItem(refused.tree, "version"));
    CHECK(!exists_in_scratch("cache/bundles/github+12"));
    cJSON_Delete(refused.tree);
    catalog_answer wrong = install_with("github:12", "{\"acceptPrimary\":\"github:99\"}");
    CHECK(flag_is(wrong, "needsUserAcceptance", 1));
    CHECK(!exists_in_scratch("cache/bundles/github+12"));
    cJSON_Delete(wrong.tree);
    catalog_answer accepted = install_with("github:12", "{\"acceptPrimary\":\"github:12\"}");
    CHECK(succeeded(accepted, "accepted"));
    CHECK(flag_is(accepted, "needsUserAcceptance", -1));
    CHECK(exists_in_scratch("cache/bundles/github+12/1.0.0/plugin.json"));
    char *record = read_scratch("cache/identities/github+12.json");
    CHECK(record != NULL && strstr(record, "\"github:12\"") != NULL && strstr(record, "github:5") == NULL);
    free(record);
    cJSON_Delete(accepted.tree);
}

static void a_mirror_installs_from_the_primary_and_deploys_its_name(void) {
    catalog_answer answer = install_with("github:6", NULL);
    CHECK(succeeded(answer, "mirror"));
    check_text(text_at(answer.tree, "id"), "github:5", "installed from the primary");
    check_text(text_at(answer.tree, "name"), "greeter", "the primary's name");
    check_text(text_at(answer.tree, "primary"), "github:5", "primary");
    CHECK(exists_in_scratch("cache/bundles/github+5/1.0.0/plugin.json"));
    CHECK(!exists_in_scratch("cache/bundles/github+6"));
    char *identity = read_scratch("plugins-github-6/greeter_identity.json");
    CHECK(identity != NULL && strstr(identity, "github:5") != NULL && strstr(identity, "greeter") != NULL);
    free(identity);
    cJSON_Delete(answer.tree);
}

static void copies_naming_different_primaries_stop_the_install(void) {
    catalog_answer answer = install_with("github:13", NULL);
    CHECK(expect_refusal(answer, "different primaries", "disagreement"));
    CHECK(!exists_in_scratch("cache/bundles/github+13"));
    CHECK(!exists_in_scratch("plugins-github-13"));
    cJSON_Delete(answer.tree);
}

static void a_folder_id_installs_from_its_primary_at_the_same_folder(void) {
    catalog_answer answer = install_with("github:6/kernel", NULL);
    CHECK(succeeded(answer, "folder through a mirror"));
    check_text(text_at(answer.tree, "id"), "github:5/kernel", "installed from the primary's folder");
    check_text(text_at(answer.tree, "primary"), "github:5", "primary is the repo");
    CHECK(exists_in_scratch("cache/bundles/github+5=kernel/1.0.0/plugin.json"));
    CHECK(!exists_in_scratch("cache/bundles/github+6=kernel"));
    char *identity = read_scratch("plugins-github-6-kernel/greeter_identity.json");
    CHECK(identity != NULL && strstr(identity, "github:5/kernel") != NULL);
    free(identity);
    char *record = read_scratch("cache/identities/github+6.json");
    CHECK(record_primary_is(record, "github:5"));
    free(record);
    cJSON_Delete(answer.tree);
}

static void a_folder_id_installs_again(void) {
    for (int attempt = 1; attempt <= 2; attempt++) {
        catalog_answer answer = install_with("github:5/kernel", NULL);
        CHECK(succeeded(answer, attempt == 1 ? "folder first" : "folder second"));
        check_text(text_at(answer.tree, "primary"), "github:5", "primary");
        CHECK(flag_is(answer, "needsUserAcceptance", -1));
        cJSON_Delete(answer.tree);
    }
}

static void a_refusal_from_the_copies_other_than_nothing_readable_stops_the_install(void) {
    catalog_answer answer = install_with("github:15", NULL);
    CHECK(answer.status == SPISOR_STATUS_FAILURE);
    CHECK(!exists_in_scratch("cache/bundles/github+15"));
    CHECK(!exists_in_scratch("plugins-github-15"));
    cJSON_Delete(answer.tree);
}

static void an_accepted_change_of_primary_replaces_the_deployed_plugin(void) {
    catalog_answer first = install_with("github:20", NULL);
    CHECK(succeeded(first, "before the move"));
    check_text(text_at(first.tree, "primary"), "github:20", "first primary");
    cJSON_Delete(first.tree);
    char *deployed = read_scratch("plugins-github-20/greeter_identity.json");
    CHECK(deployed != NULL && strstr(deployed, "github:20") != NULL);
    free(deployed);
    catalog_answer refused = install_with("github:20", NULL);
    CHECK(succeeded(refused, "after the move, unaccepted"));
    CHECK(flag_is(refused, "needsUserAcceptance", 1));
    cJSON_Delete(refused.tree);
    deployed = read_scratch("plugins-github-20/greeter_identity.json");
    CHECK(deployed != NULL && strstr(deployed, "github:20") != NULL && strstr(deployed, "github:21") == NULL);
    free(deployed);
    catalog_answer accepted = install_with("github:20", "{\"acceptPrimary\":\"github:21\"}");
    CHECK(succeeded(accepted, "after the move, accepted"));
    check_text(text_at(accepted.tree, "id"), "github:21", "now installed from the new primary");
    deployed = read_scratch("plugins-github-20/greeter_identity.json");
    CHECK(deployed != NULL && strstr(deployed, "github:21") != NULL && strstr(deployed, "twentyone") != NULL);
    free(deployed);
    char *record = read_scratch("cache/identities/github+20.json");
    CHECK(record_primary_is(record, "github:21"));
    free(record);
    cJSON_Delete(accepted.tree);
    catalog_answer again = install_with("github:20", NULL);
    CHECK(succeeded(again, "retry after acceptance"));
    CHECK(flag_is(again, "needsUserAcceptance", -1));
    cJSON_Delete(again.tree);
}

static void a_release_without_an_index_is_refused_naming_it(void) {
    catalog_answer answer = install_with("github:8", NULL);
    CHECK(expect_refusal(answer, "weivel-index.json", "no index asset"));
    cJSON_Delete(answer.tree);
}

static void refuses_a_local_id_and_a_missing_argument(void) {
    catalog_answer answer = install_with("local:greeter", NULL);
    CHECK(expect_refusal(answer, "no repo host", "local id"));
    cJSON_Delete(answer.tree);
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "id", "github:5");
    answer = call_catalog(runtime_install_by_id, argument);
    CHECK(expect_refusal(answer, "cache", "no cache"));
    cJSON_Delete(answer.tree);
}

static void run_against_the_server(void) {
    static const char repository_five[] = REPOSITORY("5", "greeter");
    static const char repository_six[] = REPOSITORY("6", "mirror");
    static const char repository_seven[] = REPOSITORY("7", "seven");
    static const char repository_eight[] = REPOSITORY("8", "eight");
    static const char repository_nine[] = REPOSITORY("9", "nine");
    static const char repository_nine_taken[] = REPOSITORY("9", "taken");
    static const char repository_ten[] = REPOSITORY("10", "ten");
    static const char repository_eleven[] = REPOSITORY("11", "eleven");
    static const char repository_twelve[] = REPOSITORY("12", "twelve");
    static const char repository_thirteen[] = REPOSITORY("13", "thirteen");
    static const char repository_fourteen[] = REPOSITORY("14", "fourteen");
    static const char repository_fifteen[] = REPOSITORY("15", "fifteen");
    static const char repository_twenty[] = REPOSITORY("20", "twenty");
    static const char repository_twenty_one[] = REPOSITORY("21", "twentyone");
    static const char twenty_before[] = "{\"primary\":\"github:20\",\"ids\":[\"github:20\"]}";
    static const char twenty_after[] = "{\"primary\":\"github:21\",\"ids\":[\"github:20\",\"github:21\"]}";
    static const char twenty_one_file[] = "{\"primary\":\"github:21\",\"ids\":[\"github:20\",\"github:21\"]}";
    static const char thirteen_file[] = "{\"primary\":\"github:13\",\"ids\":[\"github:13\",\"github:14\"]}";
    static const char fourteen_file[] = "{\"primary\":\"github:14\",\"ids\":[\"github:13\",\"github:14\"]}";
    static const char also_at_file[] = "{\"primary\":\"github:5\",\"ids\":[\"github:5\",\"github:6\"]}";
    http_route routes[] = {
        ROUTE("/repositories/5", 200, repository_five),
        ROUTE("/repositories/6", 200, repository_six),
        ROUTE("/o/greeter/HEAD/.weivel/also-at.json", 200, also_at_file),
        ROUTE("/o/mirror/HEAD/.weivel/also-at.json", 200, also_at_file),
        {"/repos/o/greeter/releases/latest", 200, NULL, release_json_good, 0, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0},
        ROUTE("/repositories/7", 200, repository_seven),
        EMPTY_ROUTE("/o/seven/HEAD/.weivel/also-at.json", 500),
        {"/repos/o/seven/releases/latest", 200, NULL, release_json_good, 0, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0},
        ROUTE("/repositories/8", 200, repository_eight),
        EMPTY_ROUTE("/o/eight/HEAD/.weivel/also-at.json", 404),
        ROUTE("/repos/o/eight/releases/latest", 200, release_json_without_index),
        {"/repositories/9", 200, NULL, repository_nine, sizeof repository_nine - 1, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 2},
        ROUTE("/repositories/9", 200, repository_nine_taken),
        EMPTY_ROUTE("/o/nine/HEAD/.weivel/also-at.json", 404),
        {"/repos/o/nine/releases/latest", 200, NULL, release_json_good, 0, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0},
        ROUTE("/repositories/10", 200, repository_ten),
        EMPTY_ROUTE("/o/ten/HEAD/.weivel/also-at.json", 404),
        {"/repos/o/ten/releases/latest", 200, NULL, release_json_tampered, 0, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0},
        ROUTE("/repositories/11", 200, repository_eleven),
        EMPTY_ROUTE("/o/eleven/HEAD/.weivel/also-at.json", 404),
        {"/repos/o/eleven/releases/latest", 200, NULL, release_json_good, 0, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0},
        ROUTE("/repositories/12", 200, repository_twelve),
        EMPTY_ROUTE("/o/twelve/HEAD/.weivel/also-at.json", 404),
        {"/repos/o/twelve/releases/latest", 200, NULL, release_json_good, 0, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0},
        {"/repos/o/mirror/releases/latest", 200, NULL, release_json_tampered, 0, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0},
        {"/repositories/15", 200, NULL, repository_fifteen, sizeof repository_fifteen - 1, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 1},
        EMPTY_ROUTE("/repositories/15", 404),
        {"/repos/o/fifteen/releases/latest", 200, NULL, release_json_good, 0, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0},
        ROUTE("/repositories/20", 200, repository_twenty),
        ROUTE("/repositories/21", 200, repository_twenty_one),
        {"/o/twenty/HEAD/.weivel/also-at.json", 200, NULL, twenty_before, sizeof twenty_before - 1, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 1},
        ROUTE("/o/twenty/HEAD/.weivel/also-at.json", 200, twenty_after),
        ROUTE("/o/twentyone/HEAD/.weivel/also-at.json", 200, twenty_one_file),
        {"/repos/o/twenty/releases/latest", 200, NULL, release_json_good, 0, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0},
        {"/repos/o/twentyone/releases/latest", 200, NULL, release_json_good, 0, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0},
        ROUTE("/repositories/13", 200, repository_thirteen),
        ROUTE("/repositories/14", 200, repository_fourteen),
        ROUTE("/o/thirteen/HEAD/.weivel/also-at.json", 200, thirteen_file),
        ROUTE("/o/fourteen/HEAD/.weivel/also-at.json", 200, fourteen_file),
        {"/repos/o/thirteen/releases/latest", 200, NULL, release_json_good, 0, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0},
    };
    for (size_t index = 0; index < sizeof routes / sizeof routes[0]; index++) {
        if (routes[index].body_length == 0 && routes[index].body != NULL && routes[index].body[0] != '\0') {
            routes[index].body_length = strlen(routes[index].body);
        }
    }
    http_fixture *server = http_fixture_start(routes, sizeof routes / sizeof routes[0]);
    CHECK(server != NULL);
    if (server == NULL) {
        return;
    }
    server_port = http_fixture_port(server);
    installs_from_the_release_index_and_checks_digests();
    install_by_id_records_the_set_and_the_primary();
    an_id_key_lands_in_its_folder_on_windows_paths();
    a_tampered_file_is_refused_and_nothing_is_deployed();
    a_name_taken_during_the_install_is_refused_and_nothing_is_installed();
    an_unreadable_start_file_installs_and_says_the_primary_is_unconfirmed();
    the_first_install_records_the_set_and_the_primary();
    an_unchanged_primary_installs_again_without_asking();
    a_changed_primary_is_refused_until_the_user_accepts_it();
    a_mirror_installs_from_the_primary_and_deploys_its_name();
    copies_naming_different_primaries_stop_the_install();
    a_folder_id_installs_from_its_primary_at_the_same_folder();
    a_folder_id_installs_again();
    a_refusal_from_the_copies_other_than_nothing_readable_stops_the_install();
    an_accepted_change_of_primary_replaces_the_deployed_plugin();
    a_release_without_an_index_is_refused_naming_it();
    refuses_a_local_id_and_a_missing_argument();
    free(http_fixture_stop(server));
}

#if !defined(_WIN32)
static void clear_proxy_environment(void) {
    const char *names[] = {"http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY", "all_proxy",
                           "ALL_PROXY",  "no_proxy",   "NO_PROXY"};
    for (size_t index = 0; index < sizeof names / sizeof names[0]; index++) {
        unsetenv(names[index]);
    }
}
#endif

int main(void) {
#if !defined(_WIN32)
    clear_proxy_environment();
#endif
    runtime_fetch_allow_plain_http(1);
    prepare_releases();
    run_against_the_server();
    return check_summary("install_by_id_test");
}
