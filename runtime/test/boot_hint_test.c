#include "updates_fixture.h"

#define HOST "forebay-host"
#define LIBRARIES "{\"runtime\":\"0.1.0\",\"koffi\":\"2.0.0\"}"

static catalog_answer read_hint(const char *host) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "cache", cache);
    cJSON_AddStringToObject(argument, "hostId", host);
    return call_catalog(runtime_boot_read, argument);
}

static catalog_answer write_hint_through_boot(const char *host, const char *libraries_json, const char *version) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "cache", cache);
    cJSON_AddStringToObject(argument, "hostId", host);
    cJSON_AddItemToObject(argument, "libraries", parsed(libraries_json));
    if (version != NULL) {
        cJSON_AddStringToObject(argument, "host", version);
    }
    return call_catalog(runtime_boot_write, argument);
}

static void install_host(const char *version) {
    list_host(HOST, version, version);
    install_listed(HOST, version);
}

/* boot-hint.ts */

static void it_writes_the_hint_as_json_stringify_writes_it_and_reads_it_back(void) {
    start_base("hint-round-trip");
    catalog_answer written = write_hint_through_boot(HOST, LIBRARIES, "1.0.0");
    expect_success(written, "round trip write");
    cJSON_Delete(written.tree);
    expect_text_in(cache, "hosts/forebay-host.json",
                   "{\n  \"libraries\": {\n    \"runtime\": \"0.1.0\",\n    \"koffi\": \"2.0.0\"\n  },\n  \"host\": "
                   "\"1.0.0\"\n}\n",
                   "round trip text");
    catalog_answer read = read_hint(HOST);
    expect_member_json(read.tree, "hint", "{\"libraries\":" LIBRARIES ",\"host\":\"1.0.0\"}", "round trip read");
    cJSON_Delete(read.tree);
    finish_base();
}

static void it_keeps_the_recorded_host_version_when_a_write_names_none(void) {
    start_base("hint-keeps-host");
    cJSON_Delete(write_hint_through_boot(HOST, LIBRARIES, "1.0.0").tree);
    cJSON_Delete(write_hint_through_boot(HOST, "{\"runtime\":\"0.2.0\",\"koffi\":\"2.0.0\"}", NULL).tree);
    expect_hint(HOST, "{\"libraries\":{\"runtime\":\"0.2.0\",\"koffi\":\"2.0.0\"},\"host\":\"1.0.0\"}", "keeps host");
    finish_base();
}

static void it_reads_no_hint_when_there_is_none_or_it_is_not_readable_json(void) {
    start_base("hint-none");
    catalog_answer none = read_hint(HOST);
    expect_success(none, "no hint");
    CHECK(none.tree != NULL && cJSON_GetObjectItemCaseSensitive(none.tree, "hint") == NULL);
    cJSON_Delete(none.tree);
    write_hint(HOST, "{ not json");
    catalog_answer corrupt = read_hint(HOST);
    CHECK(corrupt.tree != NULL && cJSON_GetObjectItemCaseSensitive(corrupt.tree, "hint") == NULL);
    cJSON_Delete(corrupt.tree);
    write_hint(HOST, "[1]");
    catalog_answer listed = read_hint(HOST);
    CHECK(listed.tree != NULL && cJSON_GetObjectItemCaseSensitive(listed.tree, "hint") == NULL);
    cJSON_Delete(listed.tree);
    finish_base();
}

static void it_reads_only_the_string_entries_of_the_libraries_and_a_string_host(void) {
    start_base("hint-strings");
    write_hint(HOST, "{\"libraries\":{\"runtime\":\"0.1.0\",\"bad\":2},\"host\":3}");
    catalog_answer read = read_hint(HOST);
    expect_member_json(read.tree, "hint", "{\"libraries\":{\"runtime\":\"0.1.0\"}}", "strings only");
    cJSON_Delete(read.tree);
    finish_base();
}

static void it_refuses_a_host_id_that_would_record_its_libraries_outside_the_hosts_folder(void) {
    start_base("hint-escape");
    expect_refusal(write_hint_through_boot("../escape", LIBRARIES, "1.0.0"), "outside", "escape write");
    expect_refusal(read_hint("../escape"), "outside", "escape read");
    expect_refusal(read_hint(""), NULL, "empty id");
    CHECK(!exists_in(scratch, "escape.json"));
    finish_base();
}

/* boot-host.ts, through boot.resolve */

static void it_resolves_the_executable_of_the_version_the_hint_names_with_the_arguments(void) {
    start_base("resolve-hinted");
    install_host("1.0.0");
    install_host("2.0.0");
    write_hint(HOST, "{\"libraries\":{},\"host\":\"1.0.0\"}");
    catalog_answer resolved = resolve_boot(HOST, "[\"launch\",\"cc\"]");
    expect_success(resolved, "hinted");
    expect_executable(resolved, HOST, "1.0.0", "hinted executable");
    expect_member_json(resolved.tree, "arguments", "[\"launch\",\"cc\"]", "hinted arguments");
    check_text(text_at(resolved.tree, "version"), "1.0.0", "hinted version");
    cJSON_Delete(resolved.tree);
    finish_base();
}

static void it_names_the_windows_executable_with_its_extension(void) {
    start_base("resolve-windows");
    list_version(HOST, "host", "1.0.0", "1.0.0", "win32-x64/forebay-host.exe", NULL);
    install_listed(HOST, "1.0.0");
    write_hint(HOST, "{\"libraries\":{},\"host\":\"1.0.0\"}");
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "cache", cache);
    cJSON_AddStringToObject(argument, "hostId", HOST);
    cJSON_AddStringToObject(argument, "platform", "win32-x64");
    catalog_answer resolved = call_catalog(runtime_boot_resolve, argument);
    expect_success(resolved, "windows");
    const char *executable = text_at(resolved.tree, "executable");
    CHECK(executable != NULL && strstr(executable, "forebay-host.exe") != NULL && spisor_file_exists(executable));
    cJSON_Delete(resolved.tree);
    finish_base();
}

static void it_refuses_when_no_host_id_is_given(void) {
    start_base("resolve-no-id");
    catalog_answer resolved = resolve_boot(NULL, NULL);
    expect_refusal(resolved, "host id", "no id");
    cJSON_Delete(resolved.tree);
    finish_base();
}

static void it_refuses_with_the_reinstall_fix_when_there_is_no_hint(void) {
    start_base("resolve-no-hint");
    install_host("1.0.0");
    catalog_answer resolved = resolve_boot(HOST, NULL);
    expect_refusal(resolved, "no host version is recorded for forebay-host", "no hint");
    expect_fix(resolved, REINSTALL_FIX, "no hint fix");
    cJSON_Delete(resolved.tree);
    finish_base();
}

static void it_refuses_with_the_reinstall_fix_when_the_hint_is_corrupt_never_picking_an_installed_version(void) {
    start_base("resolve-corrupt");
    install_host("1.0.0");
    write_hint(HOST, "{ not json");
    catalog_answer resolved = resolve_boot(HOST, NULL);
    expect_refusal(resolved, "no host version is recorded", "corrupt");
    expect_fix(resolved, REINSTALL_FIX, "corrupt fix");
    CHECK(text_at(resolved.tree, "executable") == NULL);
    cJSON_Delete(resolved.tree);
    finish_base();
}

static void it_refuses_a_hint_that_names_a_version_outside_the_bundles_folder(void) {
    start_base("resolve-outside");
    write_hint(HOST, "{\"libraries\":{},\"host\":\"../../escape\"}");
    catalog_answer resolved = resolve_boot(HOST, NULL);
    expect_refusal(resolved, "is not a version", "outside");
    expect_fix(resolved, REINSTALL_FIX, "outside fix");
    cJSON_Delete(resolved.tree);
    finish_base();
}

static void it_refuses_a_host_bundle_whose_file_no_longer_has_its_hash(void) {
    start_base("resolve-tampered");
    install_host("1.0.0");
    write_hint(HOST, "{\"libraries\":{},\"host\":\"1.0.0\"}");
    write_in(cache, "bundles/forebay-host/1.0.0/" PLATFORM "/forebay-host", "tampered");
    catalog_answer resolved = resolve_boot(HOST, NULL);
    expect_refusal(resolved, "was not started", "tampered");
    expect_fix(resolved, REINSTALL_FIX, "tampered fix");
    cJSON_Delete(resolved.tree);
    finish_base();
}

static void it_refuses_when_the_version_the_hint_names_is_not_installed(void) {
    start_base("resolve-absent");
    install_host("1.0.0");
    write_hint(HOST, "{\"libraries\":{},\"host\":\"9.9.9\"}");
    catalog_answer resolved = resolve_boot(HOST, NULL);
    expect_refusal(resolved, "is not installed", "absent");
    expect_fix(resolved, REINSTALL_FIX, "absent fix");
    cJSON_Delete(resolved.tree);
    finish_base();
}

static void it_refuses_an_installed_version_that_carries_no_executable_for_the_platform(void) {
    start_base("resolve-no-executable");
    mark_installed("bundles", HOST, "1.0.0");
    write_hint(HOST, "{\"libraries\":{},\"host\":\"1.0.0\"}");
    catalog_answer resolved = resolve_boot(HOST, NULL);
    expect_refusal(resolved, "carries no executable", "no executable");
    expect_fix(resolved, REINSTALL_FIX, "no executable fix");
    cJSON_Delete(resolved.tree);
    finish_base();
}

/* boot-entry.test.ts: what a sweep leaves */

static void it_leaves_everything_the_hint_names_after_a_sweep(void) {
    start_base("sweep-keeps-hint");
    install_host("1.0.0");
    install_host("2.0.0");
    install_host("3.0.0");
    mark_installed("libraries", "runtime", "0.1.0");
    mark_installed("libraries", "runtime", "0.2.0");
    mark_installed("libraries", "koffi", "2.0.0");
    write_hint(HOST, "{\"libraries\":{\"runtime\":\"0.1.0\",\"koffi\":\"2.0.0\"},\"host\":\"2.0.0\"}");
    record_references(HOST, "{\"run\":{},\"next\":{}}");
    catalog_answer swept = sweep_cache();
    expect_success(swept, "sweep");
    cJSON_Delete(swept.tree);
    CHECK(exists_in(cache, "bundles/forebay-host/2.0.0/" PLATFORM "/forebay-host"));
    CHECK(!exists_in(cache, "bundles/forebay-host/1.0.0"));
    CHECK(exists_in(cache, "libraries/runtime/0.1.0/" PLATFORM));
    CHECK(exists_in(cache, "libraries/koffi/2.0.0/" PLATFORM));
    catalog_answer resolved = resolve_boot(HOST, NULL);
    expect_executable(resolved, HOST, "2.0.0", "after sweep");
    cJSON_Delete(resolved.tree);
    finish_base();
}

int main(void) {
    it_writes_the_hint_as_json_stringify_writes_it_and_reads_it_back();
    it_keeps_the_recorded_host_version_when_a_write_names_none();
    it_reads_no_hint_when_there_is_none_or_it_is_not_readable_json();
    it_reads_only_the_string_entries_of_the_libraries_and_a_string_host();
    it_refuses_a_host_id_that_would_record_its_libraries_outside_the_hosts_folder();

    it_resolves_the_executable_of_the_version_the_hint_names_with_the_arguments();
    it_names_the_windows_executable_with_its_extension();
    it_refuses_when_no_host_id_is_given();
    it_refuses_with_the_reinstall_fix_when_there_is_no_hint();
    it_refuses_with_the_reinstall_fix_when_the_hint_is_corrupt_never_picking_an_installed_version();
    it_refuses_a_hint_that_names_a_version_outside_the_bundles_folder();
    it_refuses_a_host_bundle_whose_file_no_longer_has_its_hash();
    it_refuses_when_the_version_the_hint_names_is_not_installed();
    it_refuses_an_installed_version_that_carries_no_executable_for_the_platform();

    it_leaves_everything_the_hint_names_after_a_sweep();
    return check_summary("boot_hint_test");
}
