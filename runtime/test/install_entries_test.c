#include "install_fixture.h"

#include "boot_stub.h"
#include "hosts.h"
#include "install_entries.h"
#include "tree_snapshot.h"

#define LOADER_ID "entry-loader"

static const char *const MARKETPLACE_ENTRY =
    "{\"runsOnJavascriptHost\": false,"
    " \"directories\": [\"{{entryRoot}}\", \"{{entryRoot}}/.sample-plugin\", \"{{entryRoot}}/hooks\", \"{{home}}\"],"
    " \"files\": ["
    "{\"path\": \"{{entryRoot}}/.sample-plugin/marketplace.json\", \"json\": {\"name\": \"spisor\", \"owner\": {\"name\": \"spisor\"},"
    " \"description\": \"Starts the spisor host\", \"plugins\": [{\"name\": \"spisor\", \"source\": \".\", \"description\": \"Registers the spisor host each time a session starts\"}]}},"
    "{\"path\": \"{{entryRoot}}/.sample-plugin/plugin.json\", \"json\": {\"name\": \"spisor\", \"description\": \"Registers the spisor host each time a session starts\"}},"
    "{\"path\": \"{{entryRoot}}/hooks/hooks.json\", \"json\": {\"hooks\": {\"SessionStart\": [{\"hooks\": [{\"type\": \"command\","
    " \"command\": \"{{boot}}\", \"args\": [\"{{bootedHost}}\", \"hook\", \"{{loaderId}}\", \"session-start\"], \"timeout\": 30}]}]}}}],"
    " \"settings\": ["
    "{\"file\": \"{{home}}/settings.json\", \"key\": [\"enabledPlugins\", \"spisor@spisor\"], \"value\": true},"
    "{\"file\": \"{{home}}/settings.json\", \"key\": [\"extraKnownMarketplaces\", \"spisor\"],"
    " \"value\": {\"source\": {\"source\": \"directory\", \"path\": \"{{entryRoot}}\"}}}]}";

static const char *const IN_PROCESS_ENTRY =
    "{\"runsOnJavascriptHost\": true, \"directories\": [\"{{home}}\", \"{{home}}/plugins\"],"
    " \"files\": [{\"path\": \"{{home}}/plugins/spisor-{{loaderId}}.js\","
    " \"text\": \"const HINT = {{json:hint}};\\nconst LOADER_ID = {{json:loaderId}};\\nexport const SpisorHost = `${HINT}`;\\n\"}],"
    " \"settings\": []}";

/** The entry an application in these tests declares, or NULL for one that declares none. */
static cJSON *declared_entry(const char *application) {
    if (strcmp(application, "sample") == 0) {
        return parsed(MARKETPLACE_ENTRY);
    }
    return strcmp(application, "inprocess") == 0 ? parsed(IN_PROCESS_ENTRY) : NULL;
}

static const char *const HAND_WRITTEN_SETTINGS =
    "{\n"
    "    \"model\":\"opus\",  \"ratio\": 1.50,\n"
    "    \"hooks\": {\"SessionStart\": [ {\"hooks\": [{\"type\": \"command\", \"command\": \"echo \\u00e9\"}]} ]},\n"
    "    \"enabledPlugins\": {\n"
    "        \"other@market\": true\n"
    "    }\n"
    "}\n";

/** Every name under directory relative to it, joined with /, depth first in name order, as one JSON array. */
static void add_names(cJSON *names, const char *directory, const char *relative) {
    runtime_directory_listing listing;
    if (!runtime_list_directory(directory, &listing)) {
        return;
    }
    for (size_t index = 0; index < listing.count; index++) {
        char name[1024];
        snprintf(name, sizeof name, "%s%s%s", relative, relative[0] == 0 ? "" : "/", listing.entries[index].name);
        cJSON_AddItemToArray(names, cJSON_CreateString(name));
        if (listing.entries[index].is_directory) {
            char *child = joined(directory, listing.entries[index].name);
            add_names(names, child, name);
            free(child);
        }
    }
    runtime_free_directory_listing(&listing);
}

static void expect_tree(const char *directory, const char *expected_json, const char *label) {
    cJSON *names = cJSON_CreateArray();
    add_names(names, directory, "");
    expect_json(names, expected_json, label);
    cJSON_Delete(names);
}

static char *home_of(const install_base *base, const char *application) {
    char rest[256];
    snprintf(rest, sizeof rest, "homes/%s", application);
    return joined(base->user, rest);
}

/** Registers the loader of application with home, as install does before writing its entry. */
static void register_loader(const install_base *base, const char *application, const char *home) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "places", places_of(base));
    cJSON *identity = cJSON_AddObjectToObject(argument, "identity");
    cJSON_AddStringToObject(identity, "id", LOADER_ID);
    cJSON_AddStringToObject(identity, "version", "1.0.0");
    cJSON_AddStringToObject(identity, "home", home);
    cJSON_AddStringToObject(cJSON_AddObjectToObject(identity, "application"), "id", application);
    cJSON_AddFalseToObject(identity, "requiresCommand");
    cJSON_AddArrayToObject(identity, "commands");
    cJSON *provenance = cJSON_AddObjectToObject(argument, "provenance");
    cJSON_AddStringToObject(provenance, "method", "catalog");
    cJSON_AddStringToObject(provenance, "version", "1.0.0");
    cJSON_AddStringToObject(provenance, "location", base->cache);
    cJSON_AddStringToObject(provenance, "os", "linux");
    cJSON_AddStringToObject(provenance, "architecture", "x64");
    cJSON_AddStringToObject(provenance, "installedAt", "2026-11-03T00:00:00.000Z");
    cJSON_AddItemToObject(argument, "environment", environment_of(base));
    catalog_answer answer = call_catalog(runtime_hosts_register, argument);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(answer.tree);
}

static catalog_answer write_entry(const install_base *base, const char *application, const char *home) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "places", places_of(base));
    cJSON *loader = cJSON_AddObjectToObject(argument, "loader");
    cJSON_AddStringToObject(loader, "id", LOADER_ID);
    cJSON *described = cJSON_AddObjectToObject(loader, "application");
    cJSON_AddStringToObject(described, "id", application);
    cJSON_AddStringToObject(described, "label", application);
    cJSON *entry = declared_entry(application);
    if (entry != NULL) {
        cJSON_AddItemToObject(described, "entry", entry);
    }
    cJSON_AddStringToObject(loader, "home", home);
    return call_catalog(runtime_install_entry_json, argument);
}

static cJSON *removed(const install_base *base) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "places", places_of(base));
    cJSON_AddStringToObject(argument, "id", LOADER_ID);
    catalog_answer answer = call_catalog(runtime_hosts_remove, argument);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    return answer.tree;
}

static void expect_clean_remove(const install_base *base, const char *label) {
    cJSON *answer = removed(base);
    expect_json(cJSON_GetObjectItemCaseSensitive(answer, "drifted"), "[]", label);
    cJSON_Delete(answer);
}

static char *entry_root(const install_base *base) {
    char rest[256];
    snprintf(rest, sizeof rest, "entries/%s", LOADER_ID);
    return joined(base->state, rest);
}

/** "spisor": the marketplace source, as the json-key change writes it into a file indented by four. */
static char *marketplace_member(const char *root) {
    cJSON *path = cJSON_CreateString(root);
    char *quoted = cJSON_PrintUnformatted(path);
    cJSON_Delete(path);
    char *member = malloc(strlen(quoted) + 256);
    sprintf(member,
            "        \"spisor\": {\n"
            "            \"source\": {\n"
            "                \"source\": \"directory\",\n"
            "                \"path\": %s\n"
            "            }\n"
            "        }\n",
            quoted);
    cJSON_free(quoted);
    return member;
}

static void adds_only_its_two_members_to_a_hand_written_settings_json_and_gives_the_whole_file_back(void) {
    install_base base;
    start_install_base(&base, "entries", "hand-written");
    char *home = home_of(&base, "sample");
    register_loader(&base, "sample", home);
    write_in(home, "settings.json", HAND_WRITTEN_SETTINGS);
    char *before = tree_snapshot(base.user, NULL);
    catalog_answer answer = write_entry(&base, "sample", home);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answer.tree, "written")));
    cJSON_Delete(answer.tree);
    char *root = entry_root(&base);
    char *member = marketplace_member(root);
    char expected[4096];
    snprintf(expected, sizeof expected,
             "{\n"
             "    \"model\":\"opus\",  \"ratio\": 1.50,\n"
             "    \"hooks\": {\"SessionStart\": [ {\"hooks\": [{\"type\": \"command\", \"command\": \"echo \\u00e9\"}]} ]},\n"
             "    \"enabledPlugins\": {\n"
             "        \"other@market\": true,\n"
             "        \"spisor@spisor\": true\n"
             "    },\n"
             "    \"extraKnownMarketplaces\":{\n"
             "%s"
             "    }\n"
             "}\n",
             member);
    char *written = text_in(home, "settings.json");
    check_text(written, expected, "settings with the entry");
    free(written);
    expect_clean_remove(&base, "hand-written remove");
    char *after = tree_snapshot(base.user, NULL);
    check_text(after, before, "the user's home byte-identical after remove");
    CHECK(!exists_in(base.state, "entries"));
    free(after);
    free(before);
    free(member);
    free(root);
    free(home);
    finish_install_base(&base);
}

static void writes_the_declared_files_with_every_placeholder_filled(void) {
    install_base base;
    start_install_base(&base, "entries", "marketplace");
    char *home = home_of(&base, "sample");
    register_loader(&base, "sample", home);
    CHECK(spisor_make_directories(home) == 1);
    catalog_answer answer = write_entry(&base, "sample", home);
    cJSON_Delete(answer.tree);
    char *root = entry_root(&base);
    expect_tree(root,
                "[\".sample-plugin\",\".sample-plugin/marketplace.json\",\".sample-plugin/plugin.json\",\"hooks\","
                "\"hooks/hooks.json\"]",
                "marketplace tree");
    cJSON *marketplace = json_in(root, ".sample-plugin/marketplace.json");
    expect_json(cJSON_GetObjectItemCaseSensitive(marketplace, "plugins"),
                "[{\"name\":\"spisor\",\"source\":\".\",\"description\":\"Registers the spisor host each time a session starts\"}]",
                "marketplace plugins");
    CHECK(strcmp(text_at(marketplace, "name"), "spisor") == 0);
    cJSON_Delete(marketplace);
    cJSON *plugin = json_in(root, ".sample-plugin/plugin.json");
    CHECK(plugin != NULL && strcmp(text_at(plugin, "name"), "spisor") == 0);
    cJSON_Delete(plugin);
    char *boot = runtime_boot_stub_path(base.cache);
    cJSON *quoted = cJSON_CreateString(boot);
    char *boot_json = cJSON_PrintUnformatted(quoted);
    char expected[2048];
    snprintf(expected, sizeof expected,
             "{\"hooks\":{\"SessionStart\":[{\"hooks\":[{\"type\":\"command\",\"command\":%s,\"args\":[\"spisor-host\","
             "\"hook\",\"" LOADER_ID "\",\"session-start\"],\"timeout\":30}]}]}}",
             boot_json);
    cJSON *hooks = json_in(root, "hooks/hooks.json");
    expect_json(hooks, expected, "hooks");
    char *hooks_text = text_in(root, "hooks/hooks.json");
    CHECK(hooks_text != NULL && strncmp(hooks_text, "{\n  \"hooks\": {\n", 15) == 0 &&
          hooks_text[strlen(hooks_text) - 1] == '\n');
    free(hooks_text);
    cJSON_Delete(hooks);
    cJSON_free(boot_json);
    cJSON_Delete(quoted);
    free(boot);
    free(root);
    free(home);
    finish_install_base(&base);
}

static void creates_a_settings_json_that_does_not_exist_and_deletes_it_again(void) {
    install_base base;
    start_install_base(&base, "entries", "creates-settings");
    char *home = home_of(&base, "sample");
    register_loader(&base, "sample", home);
    CHECK(spisor_make_directories(home) == 1);
    catalog_answer answer = write_entry(&base, "sample", home);
    cJSON_Delete(answer.tree);
    char *root = entry_root(&base);
    cJSON *quoted = cJSON_CreateString(root);
    char *root_json = cJSON_PrintUnformatted(quoted);
    char expected[2048];
    snprintf(expected, sizeof expected,
             "{\"enabledPlugins\":{\"spisor@spisor\":true},\"extraKnownMarketplaces\":{\"spisor\":{\"source\":{\"source\":"
             "\"directory\",\"path\":%s}}}}",
             root_json);
    cJSON *settings = json_in(home, "settings.json");
    expect_json(settings, expected, "created settings");
    cJSON_Delete(settings);
    expect_clean_remove(&base, "created remove");
    expect_tree(home, "[]", "home emptied");
    CHECK(spisor_file_exists(home));
    cJSON_free(root_json);
    cJSON_Delete(quoted);
    free(root);
    free(home);
    finish_install_base(&base);
}

static void creates_an_application_home_that_does_not_exist_and_removes_it_again(void) {
    install_base base;
    start_install_base(&base, "entries", "creates-home");
    char *home = home_of(&base, "sample");
    register_loader(&base, "sample", home);
    char *homes = joined(base.user, "homes");
    CHECK(spisor_make_directories(homes) == 1);
    catalog_answer answer = write_entry(&base, "sample", home);
    cJSON_Delete(answer.tree);
    CHECK(exists_in(home, "settings.json"));
    expect_clean_remove(&base, "home remove");
    CHECK(!spisor_file_exists(home));
    free(homes);
    free(home);
    finish_install_base(&base);
}

static void records_nothing_more_when_written_twice_and_still_gives_the_file_back(void) {
    install_base base;
    start_install_base(&base, "entries", "twice");
    char *home = home_of(&base, "sample");
    register_loader(&base, "sample", home);
    write_in(home, "settings.json", HAND_WRITTEN_SETTINGS);
    catalog_answer first = write_entry(&base, "sample", home);
    catalog_answer second = write_entry(&base, "sample", home);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(first.tree, "written")) &&
          cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(first.tree, "sequences")) > 0);
    expect_json(second.tree, "{\"written\":true,\"sequences\":[]}", "second write");
    cJSON_Delete(first.tree);
    cJSON_Delete(second.tree);
    cJSON_Delete(removed(&base));
    char *text = text_in(home, "settings.json");
    check_text(text, HAND_WRITTEN_SETTINGS, "file back after two writes");
    free(text);
    free(home);
    finish_install_base(&base);
}

static void writes_one_declared_text_file_gone_with_the_directory_it_created(void) {
    install_base base;
    start_install_base(&base, "entries", "inprocess");
    char *home = home_of(&base, "inprocess");
    register_loader(&base, "inprocess", home);
    write_in(home, "sample.json", "{}\n");
    catalog_answer answer = write_entry(&base, "inprocess", home);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(answer.tree);
    expect_tree(home, "[\"plugins\",\"plugins/spisor-" LOADER_ID ".js\",\"sample.json\"]", "inprocess tree");
    char *source = text_in(home, "plugins/spisor-" LOADER_ID ".js");
    char *hint = joined(base.cache, "hosts/javascript-host.json");
    cJSON *quoted = cJSON_CreateString(hint);
    char *hint_json = cJSON_PrintUnformatted(quoted);
    char line[2048];
    snprintf(line, sizeof line, "const HINT = %s;\n", hint_json);
    CHECK(source != NULL && strstr(source, line) != NULL);
    CHECK(source != NULL && strstr(source, "export const SpisorHost") != NULL);
    CHECK(source != NULL && strstr(source, "const LOADER_ID = \"" LOADER_ID "\";") != NULL);
    expect_clean_remove(&base, "inprocess remove");
    expect_tree(home, "[\"sample.json\"]", "inprocess after remove");
    cJSON_free(hint_json);
    cJSON_Delete(quoted);
    free(hint);
    free(source);
    free(home);
    finish_install_base(&base);
}

static void keeps_a_plugin_directory_the_user_already_had(void) {
    install_base base;
    start_install_base(&base, "entries", "inprocess-own");
    char *home = home_of(&base, "inprocess");
    register_loader(&base, "inprocess", home);
    write_in(home, "plugins/mine.js", "export const Mine = async () => ({});\n");
    catalog_answer answer = write_entry(&base, "inprocess", home);
    cJSON_Delete(answer.tree);
    cJSON_Delete(removed(&base));
    expect_tree(home, "[\"plugins\",\"plugins/mine.js\"]", "user's plugin kept");
    free(home);
    finish_install_base(&base);
}

static void answers_not_written_for_an_application_declaring_no_entry(void) {
    install_base base;
    start_install_base(&base, "entries", "elsewhere");
    char *home = home_of(&base, "elsewhere");
    register_loader(&base, "elsewhere", home);
    catalog_answer answer = write_entry(&base, "elsewhere", home);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(answer.tree, "written")));
    CHECK(strstr(text_at(answer.tree, "message"), "elsewhere") != NULL && strstr(text_at(answer.tree, "fix"), "command") != NULL);
    cJSON_Delete(answer.tree);
    CHECK(!spisor_file_exists(home));
    cJSON *remove_answer = removed(&base);
    CHECK(cJSON_GetObjectItemCaseSensitive(remove_answer, "reversed")->valuedouble == 0);
    cJSON_Delete(remove_answer);
    free(home);
    finish_install_base(&base);
}

static void refuses_an_entry_whose_application_facts_were_never_checked(void) {
    install_base base;
    start_install_base(&base, "entries", "unchecked");
    char *home = home_of(&base, "unchecked");
    register_loader(&base, "unchecked", home);
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "places", places_of(&base));
    cJSON *loader = cJSON_AddObjectToObject(argument, "loader");
    cJSON_AddStringToObject(loader, "id", LOADER_ID);
    cJSON *described = cJSON_AddObjectToObject(loader, "application");
    cJSON_AddStringToObject(described, "id", "unchecked");
    cJSON_AddStringToObject(described, "label", "unchecked");
    cJSON_AddItemToObject(described, "entry", declared_entry("sample"));
    cJSON *proxy = cJSON_AddObjectToObject(described, "proxy");
    cJSON_AddNumberToObject(proxy, "placeholder", 5);
    cJSON_AddStringToObject(loader, "home", home);
    catalog_answer answer = call_catalog(runtime_install_entry_json, argument);
    CHECK(answer.status == SPISOR_STATUS_FAILURE);
    CHECK(text_at(answer.tree, "message") != NULL && strstr(text_at(answer.tree, "message"), "/application/proxy/placeholder") != NULL);
    cJSON_Delete(answer.tree);
    CHECK(!spisor_file_exists(home));
    free(home);
    finish_install_base(&base);
}

int main(void) {
    user_path_guard guard;
    guard_user_path(&guard);
    adds_only_its_two_members_to_a_hand_written_settings_json_and_gives_the_whole_file_back();
    writes_the_declared_files_with_every_placeholder_filled();
    creates_a_settings_json_that_does_not_exist_and_deletes_it_again();
    creates_an_application_home_that_does_not_exist_and_removes_it_again();
    records_nothing_more_when_written_twice_and_still_gives_the_file_back();
    writes_one_declared_text_file_gone_with_the_directory_it_created();
    keeps_a_plugin_directory_the_user_already_had();
    answers_not_written_for_an_application_declaring_no_entry();
    refuses_an_entry_whose_application_facts_were_never_checked();
    expect_user_path_unchanged(&guard);
    return check_summary("install_entries_test");
}
