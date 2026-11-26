#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache.h"
#include "spisor/allocation.h"
#include "cache_call.h"
#include "catalog_call.h"
#include "catalog_url.h"
#include "check.h"
#include "fake_forge.h"
#include "http_fixture.h"
#include "install_call.h"
#include "paths.h"
#include "service_scratch.h"
#include "sha256_file.h"
#include "transport_hooks.h"

#define PLUGIN_MANIFEST "{\"importName\":\"greeter\",\"api\":1,\"version\":\"1.0.0\",\"entry\":\"dist/index.js\"}"
#define PLUGIN_ENTRY "export default {};"
#define ROUTE(path, status, body) {path, status, NULL, body, sizeof body - 1, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0}
#define RELEASE_ROUTE(path, body) {path, 200, NULL, body, 0, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0}
#define FOLDER "git+" FAKE_ROOT

static unsigned short server_port;
static char scratch[1024];
static char release_json_good[1600];
static char release_json_tampered[1600];
static const char release_json_without_index[] = "{\"assets\":[{\"name\":\"other.zip\",\"browser_download_url\":\"https://example.test/other.zip\"}]}";

static char *in_scratch(const char *relative) {
    return joined_path(scratch, relative);
}

static void write_in_scratch(const char *relative, const char *text) {
    char *path = in_scratch(relative);
    char *parent = joined_path(path, "..");
    CHECK(spisor_make_directories(parent) == SPISOR_OK);
    CHECK(spisor_replace_file(path, (const uint8_t *)text, strlen(text)) == SPISOR_OK);
    spisor_memory_release(parent);
    spisor_memory_release(path);
}

static void sha256_of(const char *text, char hex[NAULA_SHA256_HEX_SIZE]) {
    naula_sha256 hash;
    naula_sha256_start(&hash);
    naula_sha256_add(&hash, (const uint8_t *)text, strlen(text));
    naula_sha256_hex(&hash, hex);
}

static void add_file_reference(cJSON *files, const char *key, const char *url_prefix, const char *file_name,
                               const char *text) {
    char digest[NAULA_SHA256_HEX_SIZE];
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
    char *url = NULL;
    runtime_catalog_file_url_of_path(RUNTIME_HOST_IS_WINDOWS, index_file, &url);
    CHECK(url != NULL);
    snprintf(json, size, "{\"assets\":[{\"name\":\"weivel-index.json\",\"browser_download_url\":\"%s\"}]}",
             url == NULL ? "" : url);
    spisor_memory_release(url);
    spisor_memory_release(index_file);
    spisor_memory_release(index_path);
}

static void prepare_releases(void) {
    scratch_in(scratch, sizeof scratch, "install-by-id", "world");
    write_release("release", PLUGIN_ENTRY);
    write_release("release-tampered", "export default 1;;");
    write_release_json(release_json_good, sizeof release_json_good, "release");
    write_release_json(release_json_tampered, sizeof release_json_tampered, "release-tampered");
}

/** What one install is asked: under the label's own cache and plugin folder, from the places of at. */
typedef struct install_order {
    const char *label;
    const char *id;
    const char *const *at;
    const char *extra_json;
    int aimed;
} install_order;

static catalog_answer install_as(const install_order *order) {
    char relative[256];
    snprintf(relative, sizeof relative, "%s/cache", order->label);
    char *cache = in_scratch(relative);
    snprintf(relative, sizeof relative, "%s/plugins", order->label);
    char *plugins = in_scratch(relative);
    char api_base[128];
    snprintf(api_base, sizeof api_base, "http://127.0.0.1:%u", (unsigned)server_port);
    cJSON *argument = order->extra_json == NULL ? cJSON_CreateObject() : parsed(order->extra_json);
    cJSON_AddStringToObject(argument, "id", order->id);
    cJSON *at = cJSON_AddArrayToObject(argument, "at");
    for (size_t index = 0; order->at != NULL && order->at[index] != NULL; index++) {
        cJSON_AddItemToArray(at, cJSON_CreateString(order->at[index]));
    }
    cJSON_AddStringToObject(argument, "cache", cache);
    cJSON_AddStringToObject(argument, "pluginDir", plugins);
    if (order->aimed) {
        cJSON_AddStringToObject(argument, "apiBase", api_base);
    }
    spisor_memory_release(plugins);
    spisor_memory_release(cache);
    return install_call(INSTALL_BY_ID, argument);
}

static catalog_answer install_with(const char *label, const char *id, const char *place, const char *extra_json) {
    const char *at[] = {place, NULL};
    install_order order = {label, id, at, extra_json, 1};
    return install_as(&order);
}

static int succeeded(const catalog_answer answer, const char *label) {
    int matched = answer.status == SPISOR_OK && !cJSON_HasObjectItem(answer.tree, "needsUserAcceptance") &&
                  !cJSON_HasObjectItem(answer.tree, "needsNumberAcceptance");
    if (!matched) {
        const char *message = text_at(answer.tree, "message");
        fprintf(stderr, "%s: expected an install, got status %d and \"%s\"\n", label, (int)answer.status,
                message == NULL ? "(none)" : message);
        check_failures++;
    }
    return matched;
}

static int exists_in_scratch(const char *relative) {
    char *path = in_scratch(relative);
    int exists = spisor_probe_file(path) == SPISOR_FILE_PRESENT;
    spisor_memory_release(path);
    return exists;
}

static char *read_scratch(const char *relative) {
    char *path = in_scratch(relative);
    size_t length = 0;
    char *text = (char *)spisor_read_file_reporting(path, &length, &(spisor_read_outcome){SPISOR_READ_FAILED});
    spisor_memory_release(path);
    return text;
}

static int homes_are(const catalog_answer answer, const char *expected) {
    const cJSON *homes = cJSON_GetObjectItemCaseSensitive(answer.tree, "homes");
    char *printed = cJSON_IsArray(homes) ? cJSON_PrintUnformatted(homes) : NULL;
    int matched = printed != NULL && strcmp(printed, expected) == 0;
    if (!matched) {
        fprintf(stderr, "expected homes %s, got %s\n", expected, printed == NULL ? "(none)" : printed);
        check_failures++;
    }
    cJSON_free(printed);
    return matched;
}

static int is_flagged(const catalog_answer answer, const char *member) {
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answer.tree, member));
}

static void installs_from_the_primary_naula_confirms(void) {
    fake_forge_reset();
    const char *file = fake_forge_file("greeter", FAKE_HOME("alpha") "," FAKE_HOME("bee"));
    fake_forge_add(5, "alpha", "greeter", file);
    fake_forge_add(6, "bee", "greeter", file);
    catalog_answer answer = install_with("confirmed", FAKE_ID, "forge:6", NULL);
    if (succeeded(answer, "from the primary")) {
        check_text(text_at(answer.tree, "id"), FAKE_ID, "id");
        check_text(text_at(answer.tree, "name"), "greeter", "name");
        check_text(text_at(answer.tree, "fullName"), "alpha/greeter", "the primary's owner and name");
        check_text(text_at(answer.tree, "primary"), "forge:5", "primary");
        CHECK(homes_are(answer, "[\"forge:6\",\"forge:5\"]"));
        check_text(text_at(answer.tree, "version"), "1.0.0", "version");
        check_text(text_at(answer.tree, "commit"), "abc123", "commit");
        char *index_path = in_scratch("release/weivel-index.json");
        char index_digest[NAULA_SHA256_HEX_SIZE];
        CHECK(spisor_sha256_file(index_path, index_digest) == SPISOR_OK);
        check_text(text_at(answer.tree, "sha256"), index_digest, "digest of the index");
        spisor_memory_release(index_path);
    }
    CHECK(exists_in_scratch("confirmed/cache/bundles/" FOLDER "/1.0.0/plugin.json"));
    CHECK(exists_in_scratch("confirmed/cache/bundles/" FOLDER "/1.0.0/dist/index.js"));
    CHECK(exists_in_scratch("confirmed/plugins/greeter.json"));
    CHECK(exists_in_scratch("confirmed/plugins/greeter.js"));
    char *identity = read_scratch("confirmed/plugins/greeter_identity.json");
    CHECK(identity != NULL && strstr(identity, FAKE_ID) != NULL);
    spisor_memory_release(identity);
    cJSON_Delete(answer.tree);
}

static void a_folder_id_installs_at_its_folder(void) {
    fake_forge_reset();
    fake_forge_add(5, "alpha", "greeter", fake_forge_file("greeter", FAKE_HOME("alpha")));
    catalog_answer answer = install_with("folder", FAKE_ID "/kernel", "forge:5", NULL);
    CHECK(succeeded(answer, "a folder id"));
    check_text(text_at(answer.tree, "id"), FAKE_ID "/kernel", "id");
    CHECK(exists_in_scratch("folder/cache/bundles/" FOLDER "=kernel/1.0.0/plugin.json"));
    char *identity = read_scratch("folder/plugins/greeter_identity.json");
    CHECK(identity != NULL && strstr(identity, FAKE_ID "/kernel") != NULL);
    spisor_memory_release(identity);
    cJSON_Delete(answer.tree);
}

static void an_id_key_lands_in_its_folder_on_windows_paths(void) {
    char *cache = in_scratch("folder-key/cache");
    char *directory_url = NULL;
    runtime_catalog_file_url_of_path(RUNTIME_HOST_IS_WINDOWS, scratch, &directory_url);
    char url_prefix[1600];
    snprintf(url_prefix, sizeof url_prefix, "%s/release", directory_url);
    cJSON *item = release_item(url_prefix, PLUGIN_ENTRY);
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "cache", cache);
    cJSON_AddStringToObject(argument, "id", FAKE_ID "/kernel");
    cJSON_AddItemToObject(argument, "item", item);
    cJSON_AddStringToObject(argument, "version", "1.0.0");
    catalog_answer answer = cache_call(CACHE_INSTALL, argument);
    CHECK(answer.status == SPISOR_OK);
    const char *directory = text_at(answer.tree, "directory");
    CHECK(directory != NULL && strncmp(directory, cache, strlen(cache)) == 0);
    CHECK(directory != NULL && strchr(directory + strlen(cache), ':') == NULL);
    CHECK(directory != NULL && strstr(directory, FOLDER "=kernel") != NULL);
    cJSON_Delete(answer.tree);
    spisor_memory_release(directory_url);
    spisor_memory_release(cache);
}

static void a_copy_naula_refuses_is_the_install_refusal(void) {
    fake_forge_reset();
    fake_forge_add(30, "alpha", "greeter", fake_forge_file("greeter", FAKE_HOME("alpha")));
    fake_repo *fork =
        fake_forge_add(31, "mallory", "greeter", fake_forge_file("greeter", FAKE_HOME("mallory") "," FAKE_HOME("alpha")));
    fork->fork_parent = 30;
    catalog_answer answer = install_with("copy", FAKE_ID, "forge:31", NULL);
    CHECK(expect_cache_refusal_refused(answer, "copy-of", "a fork"));
    CHECK(!exists_in_scratch("copy/cache/bundles"));
    CHECK(!exists_in_scratch("copy/plugins"));
    cJSON_Delete(answer.tree);
}

static void a_tampered_file_is_refused_and_nothing_is_deployed(void) {
    fake_forge_reset();
    fake_forge_add(10, "ten", "greeter", fake_forge_file("greeter", FAKE_HOME("ten")));
    catalog_answer answer = install_with("tampered", FAKE_ID, "forge:10", NULL);
    CHECK(expect_cache_refusal_refused(answer, "index.js", "a tampered file"));
    CHECK(!exists_in_scratch("tampered/cache/bundles/" FOLDER "/1.0.0/.installed.json"));
    CHECK(!exists_in_scratch("tampered/plugins"));
    cJSON_Delete(answer.tree);
}

static void a_name_taken_during_the_install_is_refused_and_nothing_is_installed(void) {
    fake_forge_reset();
    fake_repo *nine = fake_forge_add(9, "nine", "greeter", fake_forge_file("greeter", FAKE_HOME("nine")));
    catalog_answer measured = install_with("taken-measure", FAKE_ID, "forge:9", NULL);
    CHECK(succeeded(measured, "measuring the lookups"));
    cJSON_Delete(measured.tree);
    nine->lookups_before_rename = nine->lookups - 1;
    nine->lookups = 0;
    nine->renamed_to = "someone";
    catalog_answer answer = install_with("taken", FAKE_ID, "forge:9", NULL);
    CHECK(expect_cache_refusal_refused(answer, "when the install began", "a taken name"));
    CHECK(!exists_in_scratch("taken/cache/bundles"));
    CHECK(!exists_in_scratch("taken/plugins"));
    cJSON_Delete(answer.tree);
}

static void a_release_without_an_index_is_refused_naming_it(void) {
    fake_forge_reset();
    fake_forge_add(8, "eight", "greeter", fake_forge_file("greeter", FAKE_HOME("eight")));
    catalog_answer answer = install_with("no-index", FAKE_ID, "forge:8", NULL);
    CHECK(expect_cache_refusal_refused(answer, "weivel-index.json", "no index asset"));
    cJSON_Delete(answer.tree);
}

static void a_primary_with_no_release_reader_is_refused(void) {
    fake_forge_reset();
    fake_forge_add(5, "alpha", "greeter", fake_forge_file("greeter", FAKE_HOME("alpha")));
    const char *at[] = {"forge:5", NULL};
    install_order order = {"no-reader", FAKE_ID, at, NULL, 0};
    catalog_answer answer = install_as(&order);
    CHECK(expect_cache_refusal_refused(answer, "only from GitHub, Gitea and Forgejo", "a forge with no release reader"));
    cJSON_Delete(answer.tree);
}

/** alpha#12 is the primary; then bee#13 is, as after a planned move. */
static void a_changed_primary_installs_nothing_until_the_user_accepts_it(void) {
    fake_forge_reset();
    const char *before = fake_forge_file("greeter", FAKE_HOME("alpha") "," FAKE_HOME("bee"));
    fake_forge_add(12, "alpha", "greeter", before);
    fake_forge_add(13, "bee", "greeter", before);
    catalog_answer first = install_with("moved", FAKE_ID, "forge:12", NULL);
    CHECK(succeeded(first, "before the move"));
    check_text(text_at(first.tree, "primary"), "forge:12", "first primary");
    cJSON_Delete(first.tree);
    const char *after = fake_forge_file("greeter", FAKE_HOME("bee") "," FAKE_HOME("alpha"));
    fake_forge_reset();
    fake_forge_add(12, "alpha", "greeter", after);
    fake_forge_add(13, "bee", "greeter", after);
    catalog_answer asked = install_with("moved", FAKE_ID, "forge:12", NULL);
    CHECK(asked.status == SPISOR_OK && is_flagged(asked, "needsUserAcceptance"));
    check_text(text_at(asked.tree, "recordedPrimary"), "forge:12", "recorded primary");
    check_text(text_at(asked.tree, "primary"), "forge:13", "new primary");
    CHECK(!cJSON_HasObjectItem(asked.tree, "version"));
    cJSON_Delete(asked.tree);
    catalog_answer wrong = install_with("moved", FAKE_ID, "forge:12", "{\"acceptPrimary\":\"forge:99\"}");
    CHECK(wrong.status == SPISOR_OK && is_flagged(wrong, "needsUserAcceptance"));
    cJSON_Delete(wrong.tree);
    catalog_answer accepted = install_with("moved", FAKE_ID, "forge:12", "{\"acceptPrimary\":\"forge:13\"}");
    CHECK(succeeded(accepted, "accepted primary"));
    check_text(text_at(accepted.tree, "fullName"), "bee/greeter", "installed from the new primary");
    cJSON_Delete(accepted.tree);
    catalog_answer again = install_with("moved", FAKE_ID, "forge:12", NULL);
    CHECK(succeeded(again, "after acceptance"));
    cJSON_Delete(again.tree);
}

/**
 * One consumer confirmed bee at 20; bee is deleted and re-registered at 21, and another consumer installed through
 * it. A third consumer whose at names both of their places reads both records, which disagree on bee's number.
 */
static void a_disputed_number_installs_nothing_until_the_user_accepts_it(void) {
    fake_forge_reset();
    const char *file = fake_forge_file("greeter", FAKE_HOME("alpha") "," FAKE_HOME("bee") "," FAKE_HOME("gamma"));
    fake_forge_add(10, "alpha", "greeter", file);
    fake_repo *bee = fake_forge_add(20, "bee", "greeter", file);
    fake_forge_add(30, "gamma", "greeter", file);
    catalog_answer victim = install_with("number", FAKE_ID, "forge:10", NULL);
    CHECK(succeeded(victim, "the victim"));
    cJSON_Delete(victim.tree);
    bee->gone = 1;
    fake_forge_add(21, "bee", "greeter", file);
    const char *first_contact[] = {"forge:21", "forge:30", NULL};
    install_order contact = {"number", FAKE_ID, first_contact, "{\"acceptPrimary\":\"forge:10\"}", 1};
    catalog_answer contacted = install_as(&contact);
    cJSON_Delete(contacted.tree);
    const char *both[] = {"forge:10", "forge:30", NULL};
    install_order asking = {"number", FAKE_ID, both, NULL, 1};
    catalog_answer asked = install_as(&asking);
    CHECK(asked.status == SPISOR_OK && is_flagged(asked, "needsNumberAcceptance"));
    check_text(text_at(asked.tree, "disputedAccount"), "forge:bee", "disputed account");
    check_text(text_at(asked.tree, "recordedNumber"), "forge:20", "recorded number");
    check_text(text_at(asked.tree, "offeredNumber"), "forge:21", "offered number");
    CHECK(!cJSON_HasObjectItem(asked.tree, "version"));
    cJSON_Delete(asked.tree);
    install_order accepting = {"number", FAKE_ID, both, "{\"acceptNumber\":\"forge:21\"}", 1};
    catalog_answer accepted = install_as(&accepting);
    CHECK(succeeded(accepted, "accepted number"));
    cJSON_Delete(accepted.tree);
}

static void refuses_a_local_id_and_a_missing_argument(void) {
    catalog_answer answer = install_with("local", "local:greeter", "forge:5", NULL);
    CHECK(expect_cache_refusal_refused(answer, "no home to install from", "local id"));
    cJSON_Delete(answer.tree);
    answer = install_with("old-id", "forge:5", "forge:5", NULL);
    CHECK(expect_cache_refusal_malformed(answer, "id", "an id of the old grammar"));
    cJSON_Delete(answer.tree);
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "id", FAKE_ID);
    answer = install_call(INSTALL_BY_ID, argument);
    CHECK(expect_cache_refusal_malformed(answer, "cache", "no cache"));
    cJSON_Delete(answer.tree);
}

static void a_release_the_deadline_ended_is_refused_naming_the_deadline(void) {
    fake_forge_reset();
    fake_forge_add(16, "sixteen", "greeter", fake_forge_file("greeter", FAKE_HOME("sixteen")));
    naula_fetch_use_deadline(300);
    catalog_answer answer = install_with("deadline", FAKE_ID, "forge:16", NULL);
    naula_fetch_use_deadline(0);
    CHECK(expect_cache_refusal_refused(answer, "did not finish within 300 ms", "deadline"));
    cJSON_Delete(answer.tree);
}

static void run_against_the_server(void) {
    http_route routes[] = {
        RELEASE_ROUTE("/repos/alpha/greeter/releases/latest", release_json_good),
        RELEASE_ROUTE("/repos/bee/greeter/releases/latest", release_json_good),
        RELEASE_ROUTE("/repos/ten/greeter/releases/latest", release_json_tampered),
        RELEASE_ROUTE("/repos/nine/greeter/releases/latest", release_json_good),
        RELEASE_ROUTE("/repos/mallory/greeter/releases/latest", release_json_good),
        ROUTE("/repos/eight/greeter/releases/latest", 200, release_json_without_index),
        {"/repos/sixteen/greeter/releases/latest", 200, NULL, release_json_good, 0, HTTP_ROUTE_BODY_LENGTH, NULL, NULL,
         1500, 0},
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
    installs_from_the_primary_naula_confirms();
    a_folder_id_installs_at_its_folder();
    an_id_key_lands_in_its_folder_on_windows_paths();
    a_copy_naula_refuses_is_the_install_refusal();
    a_tampered_file_is_refused_and_nothing_is_deployed();
    a_name_taken_during_the_install_is_refused_and_nothing_is_installed();
    a_release_without_an_index_is_refused_naming_it();
    a_primary_with_no_release_reader_is_refused();
    a_changed_primary_installs_nothing_until_the_user_accepts_it();
    a_disputed_number_installs_nothing_until_the_user_accepts_it();
    refuses_a_local_id_and_a_missing_argument();
    a_release_the_deadline_ended_is_refused_naming_the_deadline();
    spisor_memory_release(http_fixture_stop(server));
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
    naula_fetch_allow_plain_http(1);
    fake_forge_install();
    prepare_releases();
    run_against_the_server();
    return check_summary("install_by_id_test");
}
