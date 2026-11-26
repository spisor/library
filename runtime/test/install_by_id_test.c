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
#include "install_call.h"
#include "paths.h"
#include "service_scratch.h"
#include "sha256_file.h"
#include "transport_hooks.h"

#define PLUGIN_MANIFEST "{\"importName\":\"greeter\",\"api\":1,\"version\":\"1.0.0\",\"entry\":\"dist/index.js\"}"
#define PLUGIN_ENTRY "export default {};"
#define FOLDER "git+" FAKE_ROOT

static char scratch[1024];
static char *index_good;
static char *index_tampered;
static char *index_good_url;
static char *index_tampered_url;

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

static char *read_scratch(const char *relative);

/** The file: url the release index in directory is offered at, freed. */
static char *index_url_of(const char *directory) {
    char *index_path = in_scratch(directory);
    char *index_file = joined_path(index_path, "weivel-index.json");
    char *url = NULL;
    runtime_catalog_file_url_of_path(RUNTIME_HOST_IS_WINDOWS, index_file, &url);
    CHECK(url != NULL);
    spisor_memory_release(index_file);
    spisor_memory_release(index_path);
    return url;
}

static void prepare_releases(void) {
    scratch_in(scratch, sizeof scratch, "install-by-id", "world");
    write_release("release", PLUGIN_ENTRY);
    write_release("release-tampered", "export default 1;;");
    index_good = read_scratch("release/weivel-index.json");
    index_tampered = read_scratch("release-tampered/weivel-index.json");
    index_good_url = index_url_of("release");
    index_tampered_url = index_url_of("release-tampered");
}

/** Adds a repo whose latest release offers the good index. */
static fake_repo *add_released(uint64_t number, const char *account, const char *file) {
    fake_repo *repo = fake_forge_add(number, account, "greeter", file);
    repo->index = index_good;
    repo->index_url = index_good_url;
    return repo;
}

/** What one install is asked: under the label's own cache and plugin folder, from the places of at. */
typedef struct install_order {
    const char *label;
    const char *id;
    const char *const *at;
    const char *extra_json;
} install_order;

static catalog_answer install_as(const install_order *order) {
    char relative[256];
    snprintf(relative, sizeof relative, "%s/cache", order->label);
    char *cache = in_scratch(relative);
    snprintf(relative, sizeof relative, "%s/plugins", order->label);
    char *plugins = in_scratch(relative);
    cJSON *argument = order->extra_json == NULL ? cJSON_CreateObject() : parsed(order->extra_json);
    cJSON_AddStringToObject(argument, "id", order->id);
    cJSON *at = cJSON_AddArrayToObject(argument, "at");
    for (size_t index = 0; order->at != NULL && order->at[index] != NULL; index++) {
        cJSON_AddItemToArray(at, cJSON_CreateString(order->at[index]));
    }
    cJSON_AddStringToObject(argument, "cache", cache);
    cJSON_AddStringToObject(argument, "pluginDir", plugins);
    spisor_memory_release(plugins);
    spisor_memory_release(cache);
    return install_call(INSTALL_BY_ID, argument);
}

static catalog_answer install_with(const char *label, const char *id, const char *place, const char *extra_json) {
    const char *at[] = {place, NULL};
    install_order order = {label, id, at, extra_json};
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
    add_released(5, "alpha", file);
    add_released(6, "bee", file);
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
    add_released(5, "alpha", fake_forge_file("greeter", FAKE_HOME("alpha")));
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
    add_released(30, "alpha", fake_forge_file("greeter", FAKE_HOME("alpha")));
    fake_repo *fork = add_released(31, "mallory", fake_forge_file("greeter", FAKE_HOME("mallory") "," FAKE_HOME("alpha")));
    fork->fork_parent = 30;
    catalog_answer answer = install_with("copy", FAKE_ID, "forge:31", NULL);
    CHECK(expect_cache_refusal_refused(answer, "copy-of", "a fork"));
    CHECK(!exists_in_scratch("copy/cache/bundles"));
    CHECK(!exists_in_scratch("copy/plugins"));
    cJSON_Delete(answer.tree);
}

static void a_tampered_file_is_refused_and_nothing_is_deployed(void) {
    fake_forge_reset();
    fake_repo *ten = fake_forge_add(10, "ten", "greeter", fake_forge_file("greeter", FAKE_HOME("ten")));
    ten->index = index_tampered;
    ten->index_url = index_tampered_url;
    catalog_answer answer = install_with("tampered", FAKE_ID, "forge:10", NULL);
    CHECK(expect_cache_refusal_refused(answer, "index.js", "a tampered file"));
    CHECK(!exists_in_scratch("tampered/cache/bundles/" FOLDER "/1.0.0/.installed.json"));
    CHECK(!exists_in_scratch("tampered/plugins"));
    cJSON_Delete(answer.tree);
}

static void a_release_without_an_index_is_refused_naming_it(void) {
    fake_forge_reset();
    fake_forge_add(8, "eight", "greeter", fake_forge_file("greeter", FAKE_HOME("eight")));
    catalog_answer answer = install_with("no-index", FAKE_ID, "forge:8", NULL);
    CHECK(expect_cache_refusal_refused(answer, "no_index_asset", "no index asset"));
    cJSON_Delete(answer.tree);
}

/** A home that cannot be heard leaves naula only its record, a STALE answer, and no release is read from one. */
static void a_stale_answer_installs_nothing(void) {
    fake_forge_reset();
    fake_repo *alpha = add_released(5, "alpha", fake_forge_file("greeter", FAKE_HOME("alpha")));
    catalog_answer first = install_with("stale", FAKE_ID, "forge:5", NULL);
    CHECK(succeeded(first, "while the home answers"));
    cJSON_Delete(first.tree);
    alpha->times_out = 1;
    catalog_answer stale = install_with("stale", FAKE_ID, "forge:5", NULL);
    CHECK(expect_cache_refusal_refused(stale, "could not be confirmed at its primary forge:5", "a stale answer"));
    cJSON_Delete(stale.tree);
}

/** alpha#12 is the primary; then bee#13 is, as after a planned move. */
static void a_changed_primary_installs_nothing_until_the_user_accepts_it(void) {
    fake_forge_reset();
    const char *before = fake_forge_file("greeter", FAKE_HOME("alpha") "," FAKE_HOME("bee"));
    add_released(12, "alpha", before);
    add_released(13, "bee", before);
    catalog_answer first = install_with("moved", FAKE_ID, "forge:12", NULL);
    CHECK(succeeded(first, "before the move"));
    check_text(text_at(first.tree, "primary"), "forge:12", "first primary");
    cJSON_Delete(first.tree);
    const char *after = fake_forge_file("greeter", FAKE_HOME("bee") "," FAKE_HOME("alpha"));
    fake_forge_reset();
    add_released(12, "alpha", after);
    add_released(13, "bee", after);
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
    add_released(10, "alpha", file);
    fake_repo *bee = add_released(20, "bee", file);
    add_released(30, "gamma", file);
    catalog_answer victim = install_with("number", FAKE_ID, "forge:10", NULL);
    CHECK(succeeded(victim, "the victim"));
    cJSON_Delete(victim.tree);
    bee->gone = 1;
    add_released(21, "bee", file);
    const char *first_contact[] = {"forge:21", "forge:30", NULL};
    install_order contact = {"number", FAKE_ID, first_contact, "{\"acceptPrimary\":\"forge:10\"}"};
    catalog_answer contacted = install_as(&contact);
    cJSON_Delete(contacted.tree);
    const char *both[] = {"forge:10", "forge:30", NULL};
    install_order asking = {"number", FAKE_ID, both, NULL};
    catalog_answer asked = install_as(&asking);
    CHECK(asked.status == SPISOR_OK && is_flagged(asked, "needsNumberAcceptance"));
    const cJSON *dispute = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(asked.tree, "disputes"), 0);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(asked.tree, "disputes")) == 1);
    check_text(text_at(dispute, "account"), "forge:bee", "disputed account");
    check_text(text_at(dispute, "recordedNumber"), "forge:20", "recorded number");
    check_text(text_at(dispute, "offeredNumber"), "forge:21", "offered number");
    check_text(text_at(dispute, "accept"), "forge:bee=21", "how to accept it");
    CHECK(!cJSON_HasObjectItem(asked.tree, "version"));
    cJSON_Delete(asked.tree);
    install_order other_account = {"number", FAKE_ID, both, "{\"acceptNumber\":\"forge:gamma=21\"}"};
    catalog_answer not_this_account = install_as(&other_account);
    CHECK(not_this_account.status == SPISOR_OK && is_flagged(not_this_account, "needsNumberAcceptance"));
    cJSON_Delete(not_this_account.tree);
    install_order accepting = {"number", FAKE_ID, both, "{\"acceptNumber\":\"forge:bee=21\"}"};
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

/** A cache whose identities folder is a file keeps no confirmation; the install still happens and says so. */
static void a_cache_naula_cannot_keep_records_in_is_a_warning(void) {
    fake_forge_reset();
    add_released(5, "alpha", fake_forge_file("greeter", FAKE_HOME("alpha")));
    write_in_scratch("unkept/cache/identities", "not a folder");
    catalog_answer answer = install_with("unkept", FAKE_ID, "forge:5", NULL);
    CHECK(succeeded(answer, "with a cache that keeps no record"));
    CHECK(text_at(answer.tree, "cacheProblem") != NULL);
    cJSON_Delete(answer.tree);
}

static void run_against_the_forge(void) {
    a_cache_naula_cannot_keep_records_in_is_a_warning();
    installs_from_the_primary_naula_confirms();
    a_folder_id_installs_at_its_folder();
    an_id_key_lands_in_its_folder_on_windows_paths();
    a_copy_naula_refuses_is_the_install_refusal();
    a_tampered_file_is_refused_and_nothing_is_deployed();
    a_release_without_an_index_is_refused_naming_it();
    a_stale_answer_installs_nothing();
    a_changed_primary_installs_nothing_until_the_user_accepts_it();
    a_disputed_number_installs_nothing_until_the_user_accepts_it();
    refuses_a_local_id_and_a_missing_argument();
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
    run_against_the_forge();
    spisor_memory_release(index_good);
    spisor_memory_release(index_tampered);
    spisor_memory_release(index_good_url);
    spisor_memory_release(index_tampered_url);
    return check_summary("install_by_id_test");
}
