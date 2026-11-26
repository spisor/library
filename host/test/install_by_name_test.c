#include "host_fixture.h"
#include "spisor/allocation.h"

#include <fcntl.h>
#include <sys/stat.h>

#include "host_commands.h"
#include "host_output.h"
#include "http_fixture.h"
#include "marketplace_lookup.h"
#include "fake_forge.h"
#include "transport_hooks.h"

#if defined(_WIN32)
#include <io.h>
#define duplicate_descriptor _dup
#define redirect_descriptor _dup2
#define close_descriptor _close
#define open_for_writing(path) _open(path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE)
#else
#define duplicate_descriptor dup
#define redirect_descriptor dup2
#define close_descriptor close
#define open_for_writing(path) open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600)
#endif

#define APPLICATION_HOST "fixture-loader"
#define MANIFEST                                                                                                    \
    "{\"importName\":\"greeter\",\"api\":1,\"version\":\"1.0.0\",\"entry\":\"dist/index.js\","                    \
    "\"dependencies\":[{\"importName\":\"left-pad\",\"name\":\"left-pad\"}]}"
#define ENTRY "export default {};"
#define ENTRY_OF(id_text, place, name_text) \
    "{\"id\":\"" id_text "\",\"at\":[\"" place "\"],\"name\":\"" name_text "\",\"fullName\":\"o/" name_text "\"}"
#define COPY_OF(entries) "{\"format\":1,\"built\":\"2026-11-10T12:00:00Z\",\"entries\":[" entries "]}"

static char fixture_root[1024];
static char release_json[1600];
static host_world world;
static char *saved_out_path;
static char *saved_err_path;
static int saved_out_descriptor;
static int saved_err_descriptor;

static void set_variable(const char *name, const char *value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static void register_application(void) {
    char *home = joined(world.base.root, "app-home");
    cJSON *registration = cJSON_CreateObject();
    cJSON_AddStringToObject(registration, "id", APPLICATION_HOST);
    cJSON_AddStringToObject(registration, "kind", "host");
    cJSON_AddStringToObject(registration, "version", "1.0.0");
    cJSON_AddStringToObject(registration, "home", home);
    char *text = cJSON_PrintUnformatted(registration);
    write_in(world.base.state, "hosts/" APPLICATION_HOST ".json", text);
    cJSON_free(text);
    cJSON_Delete(registration);
    spisor_memory_release(home);
}

static void start_world(const char *label) {
    start_world_at(&world, "install-by-name", label);
    set_variable("SPISOR_HOME", world.base.portable);
    set_variable("SPISOR_STATE", world.base.state);
    set_variable("SPISOR_CACHE", world.base.cache);
    set_variable("SPISOR_SECRETS", world.portable_secrets);
    set_variable("HOME", world.base.user);
    set_variable("USERPROFILE", world.base.user);
    set_variable("LOCALAPPDATA", world.base.root);
    register_application();
}

static void list_marketplaces(const char *const *ids, const char *const *copies, size_t count) {
    cJSON *entries = cJSON_CreateArray();
    for (size_t index = 0; index < count; index++) {
        char file[64];
        snprintf(file, sizeof file, "marketplaces/%s.json", ids[index]);
        write_in(world.base.root, file, copies[index]);
        char *path = joined(world.base.root, file);
        char *url = NULL;
        runtime_catalog_file_url_of_path(RUNTIME_HOST_IS_WINDOWS, path, &url);
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "id", ids[index]);
        cJSON_AddStringToObject(entry, "url", url);
        cJSON_AddItemToArray(entries, entry);
        spisor_memory_release(url);
        spisor_memory_release(path);
    }
    cJSON *settings = cJSON_CreateObject();
    cJSON_AddItemToObject(settings, "marketplaces", entries);
    char *text = cJSON_PrintUnformatted(settings);
    write_in(world.base.portable, "settings.json", text);
    cJSON_free(text);
    cJSON_Delete(settings);
}

static void list_one_marketplace(const char *copy) {
    const char *ids[] = {"spisor"};
    const char *copies[] = {copy};
    list_marketplaces(ids, copies, 1);
}

static void start_capture(void) {
    fflush(stdout);
    fflush(stderr);
    saved_out_path = joined(world.base.root, "captured-out.txt");
    saved_err_path = joined(world.base.root, "captured-err.txt");
    saved_out_descriptor = duplicate_descriptor(1);
    saved_err_descriptor = duplicate_descriptor(2);
    int out = open_for_writing(saved_out_path);
    int err = open_for_writing(saved_err_path);
    CHECK(out >= 0 && err >= 0 && saved_out_descriptor >= 0 && saved_err_descriptor >= 0);
    redirect_descriptor(out, 1);
    redirect_descriptor(err, 2);
    close_descriptor(out);
    close_descriptor(err);
}

static void stop_capture(char **out, char **err) {
    fflush(stdout);
    fflush(stderr);
    redirect_descriptor(saved_out_descriptor, 1);
    redirect_descriptor(saved_err_descriptor, 2);
    close_descriptor(saved_out_descriptor);
    close_descriptor(saved_err_descriptor);
    *out = text_in(saved_out_path, "");
    *err = text_in(saved_err_path, "");
    spisor_memory_release(saved_out_path);
    spisor_memory_release(saved_err_path);
}

typedef struct command_run {
    int code;
    char *out;
    char *err;
} command_run;

static command_run run_install(const char *const *arguments, int count) {
    command_run run;
    start_capture();
    run.code = host_command_install(count, (char **)arguments);
    stop_capture(&run.out, &run.err);
    return run;
}

static command_run install_with_all_flags(const char *name, const char *accept, const char *accept_primary,
                                          const char *accept_number) {
    const char *arguments[10] = {"--name", name, "--application", APPLICATION_HOST};
    int count = 4;
    if (accept != NULL) {
        arguments[count++] = "--accept";
        arguments[count++] = accept;
    }
    if (accept_primary != NULL) {
        arguments[count++] = "--accept-primary";
        arguments[count++] = accept_primary;
    }
    if (accept_number != NULL) {
        arguments[count++] = "--accept-number";
        arguments[count++] = accept_number;
    }
    return run_install(arguments, count);
}

static command_run install_with_flags(const char *name, const char *accept, const char *accept_primary) {
    return install_with_all_flags(name, accept, accept_primary, NULL);
}

static command_run install_name(const char *name, const char *accept) {
    return install_with_flags(name, accept, NULL);
}

static void free_run(command_run *run) {
    spisor_memory_release(run->out);
    spisor_memory_release(run->err);
}

static void expect_nothing_installed(const char *folder) {
    CHECK(!exists_in(world.base.cache, folder));
    CHECK(!exists_in(world.base.root, "app-home/plugin/greeter.json"));
}

static int count_fetch_folders(void) {
    runtime_directory_listing listing;
    int count = 0;
    if (runtime_list_directory(world.base.cache, &listing) == SPISOR_OK) {
        for (size_t index = 0; index < listing.count; index++) {
            count += strncmp(listing.entries[index].name, "marketplace-fetch-", 18) == 0;
        }
    }
    runtime_free_directory_listing(&listing);
    return count;
}

static char *remembered_text(void) {
    return text_in(world.base.state, "marketplace-names.json");
}

static void installs_by_name_and_prints_the_warnings(void) {
    start_world("installs");
    list_one_marketplace(COPY_OF(ENTRY_OF("git:0000000000000000000000000000000000000005", "forge:5", "greeter")));
    command_run run = install_name("greeter", NULL);
    CHECK(run.code == 0);
    expect_contains(run.out, "\"id\": \"git:0000000000000000000000000000000000000005\"", "the installed id");
    expect_contains(run.out, "\"name\": \"greeter\"", "the installed name");
    expect_contains(run.err, "warning: git:0000000000000000000000000000000000000005: left-pad: ", "the warning for the dependency by name");
    CHECK(exists_in(world.base.cache, "bundles/git+0000000000000000000000000000000000000005/1.0.0/plugin.json"));
    CHECK(exists_in(world.base.root, "app-home/plugin/greeter.json"));
    char *remembered = remembered_text();
    expect_contains(remembered, "\"spisor/greeter\": \"git:0000000000000000000000000000000000000005\"", "the name is remembered");
    spisor_memory_release(remembered);
    free_run(&run);
    command_run again = install_name("spisor/greeter", NULL);
    CHECK(again.code == 0);
    free_run(&again);
    finish_world(&world);
}

static void an_ambiguous_name_lists_the_qualified_forms_and_installs_nothing(void) {
    start_world("ambiguous");
    const char *ids[] = {"first", "second"};
    const char *copies[] = {COPY_OF(ENTRY_OF("git:0000000000000000000000000000000000000005", "forge:5", "greeter")), COPY_OF(ENTRY_OF("git:000000000000000000000000000000000000000b", "forge:11", "greeter"))};
    list_marketplaces(ids, copies, 2);
    command_run run = install_name("greeter", NULL);
    CHECK(run.code == 1);
    expect_contains(run.err, "first/greeter", "the first form");
    expect_contains(run.err, "second/greeter", "the second form");
    expect_contains(run.err, "fix: ", "its fix");
    expect_nothing_installed("bundles");
    CHECK(!exists_in(world.base.state, "marketplace-names.json"));
    free_run(&run);
    command_run qualified = install_name("second/greeter", NULL);
    CHECK(qualified.code == 0);
    expect_contains(qualified.out, "git:000000000000000000000000000000000000000b", "the qualified form installs the one it names");
    free_run(&qualified);
    finish_world(&world);
}

static void a_name_that_is_listed_nowhere_installs_nothing(void) {
    start_world("not-found");
    list_one_marketplace(COPY_OF(ENTRY_OF("git:0000000000000000000000000000000000000005", "forge:5", "greeter")));
    command_run run = install_name("absent", NULL);
    CHECK(run.code == 1);
    expect_contains(run.err, "absent is not listed", "says so");
    expect_nothing_installed("bundles");
    free_run(&run);
    command_run escaped = install_name("a\x1b[2Jb", NULL);
    CHECK(escaped.code == 1);
    expect_contains(escaped.err, "a\\x1b[2Jb is not listed", "the name, escaped");
    CHECK(strchr(escaped.err, '\x1b') == NULL);
    free_run(&escaped);
    finish_world(&world);
}

static void a_remembered_name_that_points_elsewhere_needs_the_user(void) {
    start_world("changed");
    write_in(world.base.state, "marketplace-names.json", "{\"format\":1,\"names\":{\"spisor/greeter\":\"git:0000000000000000000000000000000000000005\"}}");
    list_one_marketplace(COPY_OF(ENTRY_OF("git:000000000000000000000000000000000000000b", "forge:11", "greeter")));
    command_run run = install_name("greeter", NULL);
    CHECK(run.code == 1);
    expect_contains(run.err, "git:0000000000000000000000000000000000000005", "the remembered id");
    expect_contains(run.err, "git:000000000000000000000000000000000000000b", "the id now listed");
    expect_contains(run.err, "--accept git:000000000000000000000000000000000000000b", "how to accept");
    expect_nothing_installed("bundles");
    char *remembered = remembered_text();
    expect_contains(remembered, "git:0000000000000000000000000000000000000005", "still the old id");
    CHECK(strstr(remembered, "git:000000000000000000000000000000000000000b") == NULL);
    spisor_memory_release(remembered);
    free_run(&run);
    command_run wrong = install_name("greeter", "git:0000000000000000000000000000000000000063");
    CHECK(wrong.code == 1);
    expect_nothing_installed("bundles");
    free_run(&wrong);
    command_run accepted = install_name("greeter", "git:000000000000000000000000000000000000000b");
    CHECK(accepted.code == 0);
    CHECK(exists_in(world.base.cache, "bundles/git+000000000000000000000000000000000000000b/1.0.0/plugin.json"));
    remembered = remembered_text();
    expect_contains(remembered, "\"spisor/greeter\": \"git:000000000000000000000000000000000000000b\"", "the accepted id replaces the old one");
    spisor_memory_release(remembered);
    free_run(&accepted);
    finish_world(&world);
}

static void one_unreadable_source_does_not_stop_the_others_and_is_printed_escaped(void) {
    start_world("sources");
    const char *ids[] = {"broken", "good"};
    const char *copies[] = {"<html>not a copy</html>", COPY_OF(ENTRY_OF("git:0000000000000000000000000000000000000005", "forge:5", "greeter"))};
    list_marketplaces(ids, copies, 2);
    cJSON *settings = json_in(world.base.portable, "settings.json");
    cJSON *marketplaces = cJSON_GetObjectItemCaseSensitive(settings, "marketplaces");
    cJSON *provider = cJSON_CreateObject();
    cJSON_AddStringToObject(provider, "id", "viaplugin");
    cJSON_AddStringToObject(provider, "provider", "a\x1b[2Jb");
    cJSON_AddItemToArray(marketplaces, provider);
    char *text = cJSON_PrintUnformatted(settings);
    write_in(world.base.portable, "settings.json", text);
    cJSON_free(text);
    cJSON_Delete(settings);
    command_run run = install_name("greeter", NULL);
    CHECK(run.code == 0);
    expect_contains(run.err, "marketplace broken could not be read", "the refused source is named");
    expect_contains(run.err, "skipped marketplace viaplugin", "the provider source is named");
    CHECK(strchr(run.err, '\x1b') == NULL);
    CHECK(exists_in(world.base.cache, "bundles/git+0000000000000000000000000000000000000005/1.0.0/plugin.json"));
    CHECK(count_fetch_folders() == 0);
    free_run(&run);
    finish_world(&world);
}

static void a_failed_first_install_records_nothing(void) {
    start_world("failed");
    list_one_marketplace(COPY_OF(ENTRY_OF("git:0000000000000000000000000000000000000063", "forge:99", "greeter")));
    command_run run = install_name("greeter", NULL);
    CHECK(run.code == 1);
    CHECK(!exists_in(world.base.state, "marketplace-names.json"));
    free_run(&run);
    list_one_marketplace(COPY_OF(ENTRY_OF("git:000000000000000000000000000000000000000b", "forge:11", "greeter")));
    command_run later = install_name("greeter", NULL);
    CHECK(later.code == 0);
    free_run(&later);
    finish_world(&world);
}

static void a_bare_name_that_moves_to_another_marketplace_asks(void) {
    start_world("moved");
    list_one_marketplace(COPY_OF(ENTRY_OF("git:0000000000000000000000000000000000000005", "forge:5", "greeter")));
    command_run first = install_name("greeter", NULL);
    CHECK(first.code == 0);
    free_run(&first);
    const char *ids[] = {"second"};
    const char *copies[] = {COPY_OF(ENTRY_OF("git:000000000000000000000000000000000000000b", "forge:11", "greeter"))};
    list_marketplaces(ids, copies, 1);
    command_run moved = install_name("greeter", NULL);
    CHECK(moved.code == 1);
    expect_contains(moved.err, "git:0000000000000000000000000000000000000005", "the id it meant");
    expect_contains(moved.err, "git:000000000000000000000000000000000000000b", "the id it means now");
    expect_contains(moved.err, "--accept git:000000000000000000000000000000000000000b", "how to accept");
    CHECK(!exists_in(world.base.cache, "bundles/git+000000000000000000000000000000000000000b"));
    free_run(&moved);
    finish_world(&world);
}

/** The repos most cases install: greeter at 5 and eleven at 11, each its own repo on one home. */
static void restore_forge(void) {
    fake_forge_reset();
    fake_forge_add(5, "o", "greeter", fake_forge_file("greeter", FAKE_HOME("o")))->root =
        "0000000000000000000000000000000000000005";
    fake_forge_add(11, "o", "eleven", fake_forge_file("eleven", FAKE_HOME("o")))->root =
        "000000000000000000000000000000000000000b";
}

/** Makes eleven's homes list `first` before the other, so the first of them is the primary. */
static void list_homes_of_eleven(const char *first, const char *second) {
    char homes[256];
    snprintf(homes, sizeof homes, "{\"provider\":\"forge\",\"account\":\"%s\"},{\"provider\":\"forge\",\"account\":\"%s\"}",
             first, second);
    const char *file = fake_forge_file("greeter", homes);
    fake_forge_reset();
    fake_forge_add(11, "o", "greeter", file)->root = "000000000000000000000000000000000000000b";
    fake_forge_add(12, "p", "greeter", file)->root = "000000000000000000000000000000000000000b";
}

static void a_changed_primary_asks_with_the_two_flags_kept_apart(void) {
    start_world("primary");
    list_one_marketplace(COPY_OF(ENTRY_OF("git:000000000000000000000000000000000000000b", "forge:11", "greeter")));
    list_homes_of_eleven("o", "p");
    command_run first = install_name("greeter", NULL);
    CHECK(first.code == 0);
    free_run(&first);
    list_homes_of_eleven("p", "o");
    command_run run = install_name("greeter", NULL);
    CHECK(run.code == 1);
    expect_contains(run.err, "was confirmed with the primary forge:11", "the primary confirmed before");
    expect_contains(run.err, "--accept-primary forge:12", "how to accept the primary");
    CHECK(strchr(run.err, '\x1b') == NULL);
    free_run(&run);
    command_run wrong_flag = install_with_flags("greeter", "forge:12", NULL);
    CHECK(wrong_flag.code == 1);
    free_run(&wrong_flag);
    command_run accepted = install_with_flags("greeter", NULL, "forge:12");
    CHECK(accepted.code == 0);
    expect_contains(accepted.out, "p/greeter", "installed from the new primary");
    free_run(&accepted);
    finish_world(&world);
    restore_forge();
}

/**
 * One copy confirmed bee at 20; bee is re-registered at 21 and another copy installs through it; a third whose places
 * name both of theirs reads both records, which disagree on bee's number, so the user is asked.
 */
static void a_disputed_number_asks_with_its_own_flag(void) {
    const char *id = "git:00000000000000000000000000000000000000aa";
    start_world("number");
    const char *file = fake_forge_file("greeter", FAKE_HOME("alpha") "," FAKE_HOME("bee") "," FAKE_HOME("gamma"));
    fake_forge_reset();
    fake_forge_add(10, "alpha", "greeter", file)->root = "00000000000000000000000000000000000000aa";
    fake_repo *bee = fake_forge_add(20, "bee", "greeter", file);
    bee->root = "00000000000000000000000000000000000000aa";
    fake_forge_add(30, "gamma", "greeter", file)->root = "00000000000000000000000000000000000000aa";
    list_one_marketplace(COPY_OF(ENTRY_OF("git:00000000000000000000000000000000000000aa", "forge:10", "greeter")));
    command_run victim = install_name("greeter", NULL);
    CHECK(victim.code == 0);
    free_run(&victim);
    bee->gone = 1;
    fake_forge_add(21, "bee", "greeter", file)->root = "00000000000000000000000000000000000000aa";
    list_one_marketplace(COPY_OF("{\"id\":\"git:00000000000000000000000000000000000000aa\",\"at\":[\"forge:21\",\"forge:30\"],"
                                 "\"name\":\"greeter\",\"fullName\":\"alpha/greeter\"}"));
    command_run contact = install_with_flags("greeter", NULL, "forge:10");
    free_run(&contact);
    list_one_marketplace(COPY_OF("{\"id\":\"git:00000000000000000000000000000000000000aa\",\"at\":[\"forge:10\",\"forge:30\"],"
                                 "\"name\":\"greeter\",\"fullName\":\"alpha/greeter\"}"));
    command_run run = install_name("greeter", NULL);
    CHECK(run.code == 1);
    expect_contains(run.err, "the account forge:bee was confirmed as forge:20 and now answers as forge:21", "the dispute");
    expect_contains(run.err, "--accept-number forge:21", "how to accept the number");
    free_run(&run);
    command_run wrong_flag = install_with_flags("greeter", NULL, "forge:21");
    CHECK(wrong_flag.code == 1);
    free_run(&wrong_flag);
    command_run accepted = install_with_all_flags("greeter", NULL, NULL, "forge:21");
    CHECK(accepted.code == 0);
    expect_contains(accepted.out, id, "installed once the number is accepted");
    free_run(&accepted);
    finish_world(&world);
    restore_forge();
}

static void accept_primary_does_not_accept_a_changed_name(void) {
    start_world("flags");
    write_in(world.base.state, "marketplace-names.json", "{\"format\":1,\"names\":{\"spisor/greeter\":\"git:0000000000000000000000000000000000000005\"}}");
    list_one_marketplace(COPY_OF(ENTRY_OF("git:000000000000000000000000000000000000000b", "forge:11", "greeter")));
    command_run run = install_with_flags("greeter", NULL, "git:000000000000000000000000000000000000000b");
    CHECK(run.code == 1);
    expect_contains(run.err, "--accept git:000000000000000000000000000000000000000b", "the name still needs its own flag");
    CHECK(!exists_in(world.base.cache, "bundles/git+000000000000000000000000000000000000000b"));
    free_run(&run);
    finish_world(&world);
}

static void answers_2_for_an_accept_without_a_name_and_for_a_name_with_a_catalog(void) {
    start_world("misuse");
    const char *accept_only[] = {"--accept", "git:0000000000000000000000000000000000000005"};
    command_run run = run_install(accept_only, 2);
    CHECK(run.code == 2);
    free_run(&run);
    const char *both[] = {"--name", "greeter", "--application", APPLICATION_HOST, "--catalog", "x"};
    run = run_install(both, 6);
    CHECK(run.code == 2);
    expect_contains(run.err, "--name installs from the marketplaces", "says why");
    free_run(&run);
    const char *without_application[] = {"--name", "greeter"};
    run = run_install(without_application, 2);
    CHECK(run.code == 2);
    expect_contains(run.err, "--name needs --application <id>", "names the missing flag");
    free_run(&run);
    finish_world(&world);
}

static void write_release(void) {
    snprintf(fixture_root, sizeof fixture_root, "%s/install-by-name-%d-release", RUNTIME_SCRATCH_PATH, (int)fixture_process_id());
    start_empty_directory(fixture_root);
    write_in(fixture_root, "release/plugin.json", MANIFEST);
    write_in(fixture_root, "release/index.js", ENTRY);
    char manifest_digest[NAULA_SHA256_HEX_SIZE];
    char entry_digest[NAULA_SHA256_HEX_SIZE];
    sha256_of_text(MANIFEST, manifest_digest);
    sha256_of_text(ENTRY, entry_digest);
    char index[1024];
    snprintf(index, sizeof index,
             "{\"kind\":\"plugin\",\"latest\":\"1.0.0\",\"versions\":{\"1.0.0\":{\"commit\":\"abc123\",\"files\":{"
             "\"plugin.json\":{\"url\":\"plugin.json\",\"sha256\":\"%s\",\"size\":%d},"
             "\"dist/index.js\":{\"url\":\"index.js\",\"sha256\":\"%s\",\"size\":%d}}}}}",
             manifest_digest, (int)strlen(MANIFEST), entry_digest, (int)strlen(ENTRY));
    write_in(fixture_root, "release/weivel-index.json", index);
    char *index_path = joined(fixture_root, "release/weivel-index.json");
    char *url = NULL;
    runtime_catalog_file_url_of_path(RUNTIME_HOST_IS_WINDOWS, index_path, &url);
    snprintf(release_json, sizeof release_json, "{\"assets\":[{\"name\":\"weivel-index.json\",\"browser_download_url\":\"%s\"}]}", url);
    spisor_memory_release(url);
    spisor_memory_release(index_path);
}

static void printed_json_escapes_every_control_character(void) {
    start_world("printed-json");
    cJSON *value = cJSON_CreateObject();
    cJSON_AddStringToObject(value, "name", "a\xc2\x9b" "b\x7f" "c\x1b" "d");
    start_capture();
    host_print_json(value);
    char *out = NULL;
    char *err = NULL;
    stop_capture(&out, &err);
    expect_contains(out, "\"a\\u009bb\\u007fc\\u001bd\"", "U+009B, 0x7F and 0x1B escaped");
    CHECK(strchr(out, '\x7f') == NULL && strstr(out, "\xc2\x9b") == NULL);
    spisor_memory_release(out);
    spisor_memory_release(err);
    cJSON_Delete(value);
    finish_world(&world);
}

static void run_against_the_server(void) {
    http_route routes[] = {
        {"/repos/o/greeter/releases/latest", 200, NULL, release_json, 0, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0},
        {"/repos/o/eleven/releases/latest", 200, NULL, release_json, 0, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0},
        {"/repos/p/greeter/releases/latest", 200, NULL, release_json, 0, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0},
        {"/repos/alpha/greeter/releases/latest", 200, NULL, release_json, 0, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0},
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
    char api_base[128];
    snprintf(api_base, sizeof api_base, "http://127.0.0.1:%u", (unsigned)http_fixture_port(server));
    host_marketplace_aim_lookups(api_base);
    installs_by_name_and_prints_the_warnings();
    an_ambiguous_name_lists_the_qualified_forms_and_installs_nothing();
    a_name_that_is_listed_nowhere_installs_nothing();
    a_remembered_name_that_points_elsewhere_needs_the_user();
    one_unreadable_source_does_not_stop_the_others_and_is_printed_escaped();
    a_failed_first_install_records_nothing();
    a_bare_name_that_moves_to_another_marketplace_asks();
    a_changed_primary_asks_with_the_two_flags_kept_apart();
    a_disputed_number_asks_with_its_own_flag();
    accept_primary_does_not_accept_a_changed_name();
    answers_2_for_an_accept_without_a_name_and_for_a_name_with_a_catalog();
    printed_json_escapes_every_control_character();
    host_marketplace_aim_lookups(NULL);
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
    write_release();
    fake_forge_install();
    restore_forge();
    run_against_the_server();
    return check_summary("install_by_name_test");
}
