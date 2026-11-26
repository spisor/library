#include "updates_fixture.h"
#include "spisor/allocation.h"

#include "cache_marker.h"

/** Lays out a complete bundle version the way an install of an id key does: under the id's folder. */
static void installed_in_folder(const char *folder, const char *version) {
    char rest[512];
    snprintf(rest, sizeof rest, "bundles/%s/%s", folder, version);
    char *directory = joined(cache, rest);
    char manifest[256];
    snprintf(manifest, sizeof manifest, "{\"importName\":\"a\",\"api\":1,\"version\":\"%s\",\"entry\":\"index.js\"}", version);
    const char *contents[2] = {manifest, "export default {};"};
    const char *names[2] = {"plugin.json", "index.js"};
    cJSON *hashes = cJSON_CreateObject();
    cJSON *sizes = cJSON_CreateObject();
    for (size_t index = 0; index < 2; index++) {
        write_in(directory, names[index], contents[index]);
        char sha256[NAULA_SHA256_HEX_SIZE];
        sha256_of_text(contents[index], sha256);
        cJSON_AddStringToObject(hashes, names[index], sha256);
        cJSON_AddNumberToObject(sizes, names[index], (double)strlen(contents[index]));
    }
    CHECK(runtime_cache_write_marker(directory, hashes, sizes) == SPISOR_OK);
    cJSON_Delete(sizes);
    cJSON_Delete(hashes);
    spisor_memory_release(directory);
}

static catalog_answer call_with_cache(cache_operation operation, const char *extra_member, cJSON *extra) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "cache", cache);
    if (extra_member != NULL) {
        cJSON_AddItemToObject(argument, extra_member, extra);
    }
    return cache_call(operation, argument);
}

static void lists_an_id_key_by_its_id_and_a_name_by_its_name(void) {
    start_base("id-key-installed");
    installed_in_folder("git+0000000000000000000000000000000000000005", "1.0.0");
    installed_in_folder("git+0000000000000000000000000000000000000005", "2.0.0");
    installed_in_folder("plain", "1.0.0");
    catalog_answer answer = call_with_cache(CACHE_INSTALLED, NULL, NULL);
    expect_member_json(answer.tree, "bundles", "{\"git:0000000000000000000000000000000000000005\":[\"1.0.0\",\"2.0.0\"],\"plain\":[\"1.0.0\"]}", "installed");
    cJSON_Delete(answer.tree);
    finish_base();
}

static void locates_an_id_key_in_its_folder(void) {
    start_base("id-key-locate");
    installed_in_folder("git+0000000000000000000000000000000000000005", "1.0.0");
    catalog_answer answer = call_with_cache(CACHE_LOCATE, "plugins", parsed("{\"git:0000000000000000000000000000000000000005\":\"1.0.0\"}"));
    const cJSON *found = value_at(answer.tree, "located");
    CHECK(cJSON_GetArraySize(found) == 1);
    check_text(text_at(found->child, "id"), "git:0000000000000000000000000000000000000005", "located id");
    CHECK(text_at(found->child, "directory") != NULL && strstr(text_at(found->child, "directory"), "git+0000000000000000000000000000000000000005") != NULL);
    expect_member_json(answer.tree, "problems", "[]", "no problems");
    cJSON_Delete(answer.tree);
    finish_base();
}

static void finds_no_damage_in_an_id_key(void) {
    start_base("id-key-damaged");
    installed_in_folder("git+0000000000000000000000000000000000000005", "1.0.0");
    catalog_answer answer = call_with_cache(CACHE_DAMAGED, NULL, NULL);
    expect_member_json(answer.tree, "damaged", "[]", "damaged");
    cJSON_Delete(answer.tree);
    finish_base();
}

static void sweeps_an_id_key_keeping_what_a_reference_names_and_the_newest(void) {
    start_base("id-key-sweep");
    installed_in_folder("git+0000000000000000000000000000000000000005", "1.0.0");
    installed_in_folder("git+0000000000000000000000000000000000000005", "1.5.0");
    installed_in_folder("git+0000000000000000000000000000000000000005", "2.0.0");
    write_in(state, "bundles/host.json", "{\"run\":{\"git:0000000000000000000000000000000000000005\":\"1.0.0\"},\"next\":{}}");
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "cache", cache);
    cJSON_AddStringToObject(argument, "state", state);
    catalog_answer answer = cache_call(CACHE_SWEEP, argument);
    expect_member_json(answer.tree, "removed", "[\"git:0000000000000000000000000000000000000005@1.5.0\"]", "removed");
    cJSON_Delete(answer.tree);
    CHECK(exists_in(cache, "bundles/git+0000000000000000000000000000000000000005/1.0.0"));
    CHECK(!exists_in(cache, "bundles/git+0000000000000000000000000000000000000005/1.5.0"));
    CHECK(exists_in(cache, "bundles/git+0000000000000000000000000000000000000005/2.0.0"));
    finish_base();
}

int main(void) {
    lists_an_id_key_by_its_id_and_a_name_by_its_name();
    locates_an_id_key_in_its_folder();
    finds_no_damage_in_an_id_key();
    sweeps_an_id_key_keeping_what_a_reference_names_and_the_newest();
    return check_summary("cache_id_key_test");
}
