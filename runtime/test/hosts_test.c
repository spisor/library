#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <wchar.h>
#include <windows.h>

#include "path_entry_windows_hooks.h"
#endif

#include "boot_hint.h"
#include "catalog_call.h"
#include "catalog_time.h"
#include "changes.h"
#include "check.h"
#include "commands.h"
#include "file_times.h"
#include "holders.h"
#include "host_references.h"
#include "hosts.h"
#include "platform_key.h"
#include "processes.h"
#include "registry.h"
#include "run_records.h"
#include "service_scratch.h"

#define HOST_ID "fixture-host"
#ifdef _WIN32
#define FIXTURE_HOME "C:/fixture/home"
#else
#define FIXTURE_HOME "/fixture/home"
#endif

typedef struct hosts_scratch {
    char base[1024];
    char *state;
    char *cache;
    char *user;
    char *path;
} hosts_scratch;

static void fresh(hosts_scratch *place, const char *label) {
    scratch_in(place->base, sizeof place->base, "hosts", label);
    place->state = joined_path(place->base, "state");
    place->cache = joined_path(place->base, "cache");
    place->user = joined_path(place->base, "user");
    place->path = joined_path(place->base, "path");
    CHECK(spisor_make_directories(place->state) == 1 && spisor_make_directories(place->cache) == 1);
    CHECK(spisor_make_directories(place->user) == 1 && spisor_make_directories(place->path) == 1);
    free(touch_file(place->user, ".profile", "# profile\n", 0644));
}

static cJSON *environment_of(const hosts_scratch *place) {
    cJSON *environment = cJSON_CreateObject();
    cJSON_AddStringToObject(environment, "PATH", place->path);
    cJSON_AddStringToObject(environment, "HOME", place->user);
    cJSON_AddStringToObject(environment, "USERPROFILE", place->user);
    cJSON_AddStringToObject(environment, "SHELL", "/bin/sh");
    return environment;
}

static cJSON *places_of(const hosts_scratch *place) {
    cJSON *places = cJSON_CreateObject();
    cJSON_AddStringToObject(places, "state", place->state);
    cJSON_AddStringToObject(places, "cache", place->cache);
    return places;
}

static cJSON *provenance_at(const char *location, const char *installed_at) {
    cJSON *provenance = cJSON_CreateObject();
    cJSON_AddStringToObject(provenance, "method", "npm");
    cJSON_AddStringToObject(provenance, "version", "1.0.0");
    cJSON_AddStringToObject(provenance, "location", location);
    cJSON_AddStringToObject(provenance, "os", "linux");
    cJSON_AddStringToObject(provenance, "architecture", "x64");
    cJSON_AddStringToObject(provenance, "installedAt", installed_at);
    return provenance;
}

static cJSON *identity_of(const char *id) {
    cJSON *identity = cJSON_CreateObject();
    cJSON_AddStringToObject(identity, "id", id);
    cJSON_AddStringToObject(identity, "version", "1.0.0");
    cJSON_AddStringToObject(identity, "home", FIXTURE_HOME);
    cJSON_AddFalseToObject(identity, "requiresCommand");
    cJSON_AddArrayToObject(identity, "commands");
    return identity;
}

static void add_command(const hosts_scratch *place, cJSON *identity, const char *name) {
    char *target = joined_path(place->base, "bin/thost");
    cJSON *command = cJSON_CreateObject();
    cJSON_AddStringToObject(command, "name", name);
    cJSON_AddStringToObject(command, "target", target);
    cJSON_AddArrayToObject(command, "arguments");
    cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(identity, "commands"), command);
    free(target);
}

static catalog_answer register_with(const hosts_scratch *place, cJSON *identity, cJSON *provenance,
                                    cJSON *environment) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddItemToObject(request, "places", places_of(place));
    cJSON_AddItemToObject(request, "identity", identity);
    cJSON_AddItemToObject(request, "provenance", provenance);
    cJSON_AddItemToObject(request, "environment", environment);
    return call_catalog(runtime_hosts_register, request);
}

static void register_existing(const hosts_scratch *place, const char *id, const char *location) {
    catalog_answer answer =
        register_with(place, identity_of(id), provenance_at(location, "2026-01-01T00:00:00Z"), environment_of(place));
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(answer.tree);
}

static void register_present(const hosts_scratch *place, const char *id) {
    char *location = touch_file(place->base, "install.txt", "x", 0644);
    register_existing(place, id, location);
    free(location);
}

static cJSON *stored_registration(const hosts_scratch *place, const char *id) {
    cJSON *registration = NULL;
    runtime_registry_answer loaded = runtime_registry_load(place->state, id, &registration);
    CHECK(loaded == RUNTIME_REGISTRY_OK || loaded == RUNTIME_REGISTRY_ABSENT);
    return registration;
}

static void reverse_host(const hosts_scratch *place, const char *id) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddItemToObject(request, "places", places_of(place));
    cJSON_AddStringToObject(request, "host", id);
    catalog_answer answer = call_catalog(runtime_changes_reverse_json, request);
    cJSON_Delete(answer.tree);
}

static void discard(hosts_scratch *place) {
    reverse_host(place, HOST_ID);
    free(place->state);
    free(place->cache);
    free(place->user);
    free(place->path);
}

static void check_same(const cJSON *actual, const cJSON *expected, const char *label) {
    if (!cJSON_Compare(actual, expected, 1)) {
        char *left = actual == NULL ? NULL : cJSON_PrintUnformatted(actual);
        char *right = expected == NULL ? NULL : cJSON_PrintUnformatted(expected);
        fprintf(stderr, "%s: expected %s, got %s\n", label, right == NULL ? "(null)" : right,
                left == NULL ? "(null)" : left);
        cJSON_free(left);
        cJSON_free(right);
        check_failures++;
    }
}

static void refuses_an_environment_with_no_user_home_before_registering_anything(void) {
    hosts_scratch place;
    fresh(&place, "nohome");
    cJSON *identity = identity_of(HOST_ID);
    add_command(&place, identity, "thost");
    cJSON *environment = environment_of(&place);
    cJSON_DeleteItemFromObject(environment, "HOME");
    cJSON_DeleteItemFromObject(environment, "USERPROFILE");
    char *location = joined_path(place.base, "install");
    catalog_answer answer = register_with(&place, identity, provenance_at(location, "2026-01-01T00:00:00Z"), environment);
    const char *message = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(answer.tree, "message"));
    CHECK(answer.status == SPISOR_STATUS_FAILURE && message != NULL && strstr(message, "HOME nor USERPROFILE") != NULL);
    cJSON_Delete(answer.tree);
    cJSON *registration = stored_registration(&place, HOST_ID);
    CHECK(registration == NULL);
    cJSON_Delete(registration);
    free(location);
    discard(&place);
}

static void refuses_an_identity_with_no_home_before_registering_anything(void) {
    hosts_scratch place;
    fresh(&place, "noidentityhome");
    cJSON *identity = identity_of(HOST_ID);
    cJSON_DeleteItemFromObject(identity, "home");
    char *location = joined_path(place.base, "install");
    catalog_answer answer =
        register_with(&place, identity, provenance_at(location, "2026-01-01T00:00:00Z"), environment_of(&place));
    const char *message = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(answer.tree, "message"));
    CHECK(answer.status == SPISOR_STATUS_FAILURE && message != NULL && strstr(message, "home") != NULL);
    cJSON_Delete(answer.tree);
    cJSON *registration = stored_registration(&place, HOST_ID);
    CHECK(registration == NULL);
    cJSON_Delete(registration);
    free(location);
    discard(&place);
}

static void records_the_provenance_of_the_first_start(void) {
    hosts_scratch place;
    fresh(&place, "first");
    char *location = joined_path(place.base, "install");
    cJSON *first = provenance_at(location, "2026-01-01T00:00:00Z");
    catalog_answer answer = register_with(&place, identity_of(HOST_ID), cJSON_Duplicate(first, 1), environment_of(&place));
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    check_same(cJSON_GetObjectItemCaseSensitive(answer.tree, "provenance"), first, "the answered provenance");
    cJSON *registration = stored_registration(&place, HOST_ID);
    check_same(cJSON_GetObjectItemCaseSensitive(registration, "provenance"), first, "the stored provenance");
    cJSON_Delete(registration);
    cJSON_Delete(answer.tree);
    cJSON_Delete(first);
    free(location);
    discard(&place);
}

static void keeps_the_first_provenance_when_a_later_start_gives_a_later_installed_at(void) {
    hosts_scratch place;
    fresh(&place, "later");
    char *location = joined_path(place.base, "install");
    cJSON *first = provenance_at(location, "2026-01-01T00:00:00Z");
    catalog_answer answer = register_with(&place, identity_of(HOST_ID), cJSON_Duplicate(first, 1), environment_of(&place));
    cJSON_Delete(answer.tree);
    answer = register_with(&place, identity_of(HOST_ID), provenance_at(location, "2026-02-02T00:00:00Z"),
                           environment_of(&place));
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    check_same(cJSON_GetObjectItemCaseSensitive(answer.tree, "provenance"), first, "the answered provenance");
    cJSON *registration = stored_registration(&place, HOST_ID);
    check_same(cJSON_GetObjectItemCaseSensitive(registration, "provenance"), first, "the stored provenance");
    cJSON_Delete(registration);
    cJSON_Delete(answer.tree);
    cJSON_Delete(first);
    free(location);
    discard(&place);
}

static void answers_the_same_command_name_on_a_second_start_and_requests_nothing_new(void) {
    hosts_scratch place;
    fresh(&place, "again");
    char *location = joined_path(place.base, "install");
    cJSON *identity = identity_of(HOST_ID);
    add_command(&place, identity, "thost");
    catalog_answer first = register_with(&place, cJSON_Duplicate(identity, 1), provenance_at(location, "2026-01-01T00:00:00Z"),
                                         environment_of(&place));
    catalog_answer second = register_with(&place, identity, provenance_at(location, "2026-01-01T00:00:00Z"),
                                          environment_of(&place));
    CHECK(first.status == SPISOR_STATUS_SUCCESS && second.status == SPISOR_STATUS_SUCCESS);
    const cJSON *first_commands = cJSON_GetObjectItemCaseSensitive(first.tree, "commands");
    CHECK(cJSON_GetArraySize(first_commands) == 1);
    check_same(cJSON_GetObjectItemCaseSensitive(second.tree, "commands"), first_commands, "the second start's commands");
    cJSON *registration = stored_registration(&place, HOST_ID);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(registration, "commands")) == 1);
    cJSON_Delete(registration);
    cJSON_Delete(first.tree);
    cJSON_Delete(second.tree);
    free(location);
    discard(&place);
}

static void records_how_a_launcher_starts_the_host(void) {
    hosts_scratch place;
    fresh(&place, "start");
    char *location = joined_path(place.base, "install");
    cJSON *identity = identity_of(HOST_ID);
    cJSON *start = cJSON_AddObjectToObject(identity, "start");
    cJSON_AddStringToObject(start, "runtime", "javascript");
    cJSON_AddStringToObject(start, "entry", "dist/host.js");
    catalog_answer answer = register_with(&place, identity, provenance_at(location, "2026-01-01T00:00:00Z"),
                                          environment_of(&place));
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(answer.tree);
    cJSON *registration = stored_registration(&place, HOST_ID);
    cJSON *expected = parsed("{\"runtime\": \"javascript\", \"entry\": \"dist/host.js\"}");
    check_same(cJSON_GetObjectItemCaseSensitive(registration, "start"), expected, "the recorded start");
    cJSON_Delete(expected);
    cJSON_Delete(registration);
    free(location);
    discard(&place);
}

static void refuses_a_start_with_an_unknown_runtime_or_an_entry_outside_the_bundle(void) {
    const char *starts[] = {"{\"runtime\": \"python\", \"entry\": \"host.py\"}", "{\"runtime\": \"native\", \"entry\": \"\"}",
                            "{\"runtime\": \"native\", \"entry\": \"/usr/bin/host\"}",
                            "{\"runtime\": \"jvm\", \"entry\": \"../other/host.jar\"}", "\"native\""};
    for (size_t index = 0; index < sizeof starts / sizeof starts[0]; index++) {
        hosts_scratch place;
        fresh(&place, "badstart");
        char *location = joined_path(place.base, "install");
        cJSON *identity = identity_of(HOST_ID);
        cJSON_AddItemToObject(identity, "start", parsed(starts[index]));
        catalog_answer answer = register_with(&place, identity, provenance_at(location, "2026-01-01T00:00:00Z"),
                                              environment_of(&place));
        if (answer.status != SPISOR_STATUS_FAILURE) {
            fprintf(stderr, "the start %s was not refused\n", starts[index]);
            check_failures++;
        }
        cJSON_Delete(answer.tree);
        cJSON *registration = stored_registration(&place, HOST_ID);
        CHECK(registration == NULL);
        cJSON_Delete(registration);
        free(location);
        discard(&place);
    }
}

static void reports_a_refused_command_for_a_host_that_can_run_without_one(void) {
    hosts_scratch place;
    fresh(&place, "refusedcommand");
    char *location = joined_path(place.base, "install");
    cJSON *identity = identity_of(HOST_ID);
    add_command(&place, identity, "thost");
    cJSON *environment = environment_of(&place);
    cJSON_DeleteItemFromObject(environment, "PATH");
    catalog_answer answer = register_with(&place, identity, provenance_at(location, "2026-01-01T00:00:00Z"), environment);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(answer.tree, "commands")) == 0);
    const cJSON *refused = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(answer.tree, "refusedCommands"), 0);
    check_text(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(refused, "name")), "thost", "the refused name");
    CHECK(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(refused, "detail")) &&
          cJSON_IsString(cJSON_GetObjectItemCaseSensitive(refused, "fix")));
    cJSON_Delete(answer.tree);
    cJSON *registration = stored_registration(&place, HOST_ID);
    CHECK(registration != NULL);
    cJSON_Delete(registration);
    free(location);
    discard(&place);
}

static void refuses_a_host_that_requires_the_command_it_was_refused(void) {
    hosts_scratch place;
    fresh(&place, "requires");
    char *location = joined_path(place.base, "install");
    cJSON *identity = identity_of(HOST_ID);
    cJSON_ReplaceItemInObject(identity, "requiresCommand", cJSON_CreateTrue());
    add_command(&place, identity, "thost");
    cJSON *environment = environment_of(&place);
    cJSON_DeleteItemFromObject(environment, "PATH");
    catalog_answer answer = register_with(&place, identity, provenance_at(location, "2026-01-01T00:00:00Z"), environment);
    CHECK(answer.status == SPISOR_STATUS_FAILURE &&
          cJSON_IsString(cJSON_GetObjectItemCaseSensitive(answer.tree, "message")));
    cJSON_Delete(answer.tree);
    free(location);
    discard(&place);
}

static catalog_answer remove_host(const hosts_scratch *place, const char *id) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddItemToObject(request, "places", places_of(place));
    cJSON_AddStringToObject(request, "id", id);
    return call_catalog(runtime_hosts_remove, request);
}

static cJSON *listed(const hosts_scratch *place) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddItemToObject(request, "places", places_of(place));
    catalog_answer answer = call_catalog(runtime_hosts_list, request);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    cJSON *hosts = cJSON_DetachItemFromObject(answer.tree, "hosts");
    cJSON_Delete(answer.tree);
    return hosts;
}

static int lists_only(const hosts_scratch *place, const char *id) {
    cJSON *hosts = listed(place);
    const cJSON *registration = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(hosts, 0), "registration");
    const char *listed_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(registration, "id"));
    int only = id == NULL ? cJSON_GetArraySize(hosts) == 0
                          : cJSON_GetArraySize(hosts) == 1 && listed_id != NULL && strcmp(listed_id, id) == 0;
    cJSON_Delete(hosts);
    return only;
}

static void apply_file_change(const hosts_scratch *place, const char *path) {
    cJSON *change = cJSON_CreateObject();
    cJSON_AddStringToObject(change, "kind", "file");
    cJSON_AddStringToObject(change, "path", path);
    cJSON_AddStringToObject(change, "content", "hi");
    runtime_changes_outcome outcome = runtime_changes_apply(place->state, HOST_ID, change);
    CHECK(outcome.answer == RUNTIME_CHANGES_RECORDED);
    cJSON_Delete(change);
}

static void removes_a_host_by_reversing_its_changes_and_then_forgetting_it(void) {
    hosts_scratch place;
    fresh(&place, "remove");
    register_present(&place, HOST_ID);
    char *changed = joined_path(place.base, "changed.txt");
    apply_file_change(&place, changed);
    CHECK(spisor_file_exists(changed));
    catalog_answer answer = remove_host(&place, HOST_ID);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    cJSON *expected = parsed("{\"reversed\": 1, \"drifted\": [], \"forgotten\": true}");
    check_same(answer.tree, expected, "the removal");
    cJSON_Delete(expected);
    cJSON_Delete(answer.tree);
    CHECK(!spisor_file_exists(changed));
    CHECK(lists_only(&place, NULL));
    free(changed);
    discard(&place);
}

/** Rewrites the kind of the host's first change to one no runtime can reverse, so its reversal is refused. */
static void make_the_first_change_irreversible(const hosts_scratch *place) {
    cJSON *registration = stored_registration(place, HOST_ID);
    cJSON *first = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(registration, "changes"), 0);
    CHECK(first != NULL);
    cJSON_ReplaceItemInObject(first, "kind", cJSON_CreateString("unknown"));
    CHECK(runtime_registry_store(place->state, registration) == RUNTIME_REGISTRY_OK);
    cJSON_Delete(registration);
}

static void leaves_the_registration_and_its_references_in_place_when_the_reverse_is_refused(void) {
    hosts_scratch place;
    fresh(&place, "stuck");
    register_present(&place, HOST_ID);
    char *changed = joined_path(place.base, "changed.txt");
    apply_file_change(&place, changed);
    make_the_first_change_irreversible(&place);
    runtime_host_references references = {parsed("{\"a\": \"1.0.0\"}"), parsed("{}"), NULL, NULL};
    char *problem = NULL;
    CHECK(runtime_record_host_references(place.state, HOST_ID, &references, &problem));
    runtime_free_host_references(&references);
    catalog_answer answer = remove_host(&place, HOST_ID);
    CHECK(answer.status == SPISOR_STATUS_FAILURE &&
          cJSON_IsString(cJSON_GetObjectItemCaseSensitive(answer.tree, "fix")));
    cJSON_Delete(answer.tree);
    CHECK(lists_only(&place, HOST_ID));
    char *recorded = runtime_host_references_path(place.state, HOST_ID, &problem);
    CHECK(recorded != NULL && spisor_file_exists(recorded));
    free(recorded);
    cJSON *registration = stored_registration(&place, HOST_ID);
    cJSON *first = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(registration, "changes"), 0);
    cJSON_ReplaceItemInObject(first, "kind", cJSON_CreateString("file"));
    CHECK(runtime_registry_store(place.state, registration) == RUNTIME_REGISTRY_OK);
    cJSON_Delete(registration);
    free(changed);
    discard(&place);
}

static void answers_as_orphans_the_hosts_whose_install_location_is_gone(void) {
    hosts_scratch place;
    fresh(&place, "orphans");
    register_present(&place, HOST_ID);
    char *missing = joined_path(place.base, "missing");
    register_existing(&place, "orphan-host", missing);
    cJSON *hosts = listed(&place);
    CHECK(cJSON_GetArraySize(hosts) == 2);
    int orphans = 0;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, hosts) {
        const char *id =
            cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(entry, "registration"), "id"));
        int orphan = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "orphan"));
        orphans += orphan;
        CHECK(id != NULL && orphan == (strcmp(id, "orphan-host") == 0));
    }
    CHECK(orphans == 1);
    cJSON_Delete(hosts);
    free(missing);
    discard(&place);
}

static cJSON *command_request(const hosts_scratch *place) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddItemToObject(request, "places", places_of(place));
    cJSON_AddStringToObject(request, "host", HOST_ID);
    return request;
}

static const char *answered_name(catalog_answer *answer) {
    CHECK(answer->status == SPISOR_STATUS_SUCCESS);
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(answer->tree, "name"));
}

static void adds_renames_and_deletes_a_command(void) {
    hosts_scratch place;
    fresh(&place, "commands");
    register_present(&place, HOST_ID);
    char *target = joined_path(place.base, "bin/tool");
    cJSON *request = command_request(&place);
    cJSON_AddItemToObject(request, "environment", environment_of(&place));
    cJSON_AddStringToObject(request, "home", place.user);
    cJSON_AddStringToObject(request, "name", "tool");
    cJSON_AddStringToObject(request, "target", target);
    catalog_answer answer = call_catalog(runtime_commands_add, request);
    check_text(answered_name(&answer), "tool", "the added command");
    cJSON_Delete(answer.tree);
    request = command_request(&place);
    cJSON_AddItemToObject(request, "environment", environment_of(&place));
    cJSON_AddStringToObject(request, "from", "tool");
    cJSON_AddStringToObject(request, "to", "tool-renamed");
    answer = call_catalog(runtime_commands_rename, request);
    check_text(answered_name(&answer), "tool-renamed", "the renamed command");
    cJSON_Delete(answer.tree);
    request = command_request(&place);
    cJSON_AddStringToObject(request, "name", "tool-renamed");
    answer = call_catalog(runtime_commands_delete, request);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(answer.tree);
    cJSON *registration = stored_registration(&place, HOST_ID);
    const char *state = NULL;
    const cJSON *command;
    cJSON_ArrayForEach(command, cJSON_GetObjectItemCaseSensitive(registration, "commands")) {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(command, "name"));
        if (name != NULL && strcmp(name, "tool-renamed") == 0) {
            state = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(command, "state"));
        }
    }
    check_text(state, "deleted", "the deleted command's state");
    cJSON_Delete(registration);
    free(target);
    discard(&place);
}

static void record_references(const hosts_scratch *place, const char *id) {
    runtime_host_references references = {parsed("{\"a\": \"1.0.0\"}"), parsed("{}"), NULL, NULL};
    char *problem = NULL;
    CHECK(runtime_record_host_references(place->state, id, &references, &problem));
    free(problem);
    runtime_free_host_references(&references);
}

static int references_recorded(const hosts_scratch *place, const char *id) {
    char *problem = NULL;
    char *path = runtime_host_references_path(place->state, id, &problem);
    int recorded = path != NULL && spisor_file_exists(path);
    free(path);
    free(problem);
    return recorded;
}

static void deletes_the_references_file_and_leaves_another_hosts(void) {
    hosts_scratch place;
    fresh(&place, "references");
    register_present(&place, HOST_ID);
    record_references(&place, HOST_ID);
    record_references(&place, "other-host");
    catalog_answer answer = remove_host(&place, HOST_ID);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answer.tree, "forgotten")));
    cJSON_Delete(answer.tree);
    CHECK(!references_recorded(&place, HOST_ID));
    CHECK(references_recorded(&place, "other-host"));
    discard(&place);
}

static void deletes_every_run_record_of_the_host_with_its_references(void) {
    hosts_scratch place;
    fresh(&place, "runs");
    register_present(&place, HOST_ID);
    cJSON *run = parsed("{\"a\": \"1.0.0\"}");
    char *record = runtime_record_run(place.state, HOST_ID, run, RUNTIME_DEFAULT_CHECK_MILLISECONDS);
    CHECK(record != NULL);
    cJSON *runs = runtime_live_runs(place.state, runtime_now_milliseconds());
    CHECK(cJSON_GetArraySize(runs) == 1);
    cJSON_Delete(runs);
    catalog_answer answer = remove_host(&place, HOST_ID);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(answer.tree);
    runs = runtime_live_runs(place.state, runtime_now_milliseconds());
    CHECK(runs != NULL && cJSON_GetArraySize(runs) == 0);
    cJSON_Delete(runs);
    char *folder = joined_path(place.state, "bundles/runs/" HOST_ID);
    CHECK(!spisor_file_exists(folder));
    free(folder);
    free(record);
    cJSON_Delete(run);
    discard(&place);
}

static void deletes_the_holders_file_of_the_host(void) {
    hosts_scratch place;
    fresh(&place, "holders");
    register_present(&place, HOST_ID);
    char *problem = NULL;
    char *holders = runtime_holders_path(place.state, HOST_ID, ".json", &problem);
    char *other = runtime_holders_path(place.state, "other-host", ".json", &problem);
    char *folder = runtime_processes_folder(place.state);
    CHECK(holders != NULL && other != NULL && folder != NULL && spisor_make_directories(folder) == 1);
    runtime_holders_record record = runtime_no_holders();
    CHECK(runtime_add_holder(&record, 1.0));
    CHECK(runtime_write_holders(holders, &record) && runtime_write_holders(other, &record));
    runtime_free_holders(&record);
    catalog_answer answer = remove_host(&place, HOST_ID);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(answer.tree);
    CHECK(!spisor_file_exists(holders));
    CHECK(spisor_file_exists(other));
    char *lock = runtime_holders_path(place.state, HOST_ID, ".lock", &problem);
    CHECK(lock != NULL && !spisor_file_exists(lock));
    free(lock);
    free(folder);
    free(other);
    free(holders);
    discard(&place);
}

static void deletes_the_boot_hint_of_the_host_and_leaves_another_hosts(void) {
    hosts_scratch place;
    fresh(&place, "hint");
    register_present(&place, HOST_ID);
    cJSON *libraries = cJSON_CreateObject();
    cJSON_AddStringToObject(libraries, "runtime", "0.1.0");
    char *problem = NULL;
    CHECK(runtime_write_boot_hint(place.cache, HOST_ID, libraries, "1.0.0", &problem));
    CHECK(runtime_write_boot_hint(place.cache, "other-host", libraries, "1.0.0", &problem));
    cJSON_Delete(libraries);
    char *hint = runtime_boot_hint_path(place.cache, HOST_ID, &problem);
    char *other = runtime_boot_hint_path(place.cache, "other-host", &problem);
    CHECK(hint != NULL && spisor_file_exists(hint) && other != NULL);
    catalog_answer answer = remove_host(&place, HOST_ID);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(answer.tree);
    CHECK(!spisor_file_exists(hint));
    CHECK(spisor_file_exists(other));
    free(problem);
    free(other);
    free(hint);
    discard(&place);
}

static void refuses_to_remove_a_host_id_that_is_no_plain_file_name(void) {
    hosts_scratch place;
    fresh(&place, "plainid");
    catalog_answer answer = remove_host(&place, "../escape");
    CHECK(answer.status == SPISOR_STATUS_FAILURE);
    cJSON_Delete(answer.tree);
    discard(&place);
}

static catalog_answer provenance_answer(const hosts_scratch *place, const char *origin, cJSON *environment,
                                        int with_now) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddItemToObject(request, "origin", parsed(origin));
    cJSON_AddItemToObject(request, "application", parsed("{\"binary\": \"claude\"}"));
    cJSON_AddStringToObject(request, "home", place->user);
    cJSON_AddItemToObject(request, "environment", environment);
    if (with_now) {
        cJSON_AddNumberToObject(request, "now", 1767225600123.0);
    }
    return call_catalog(runtime_hosts_provenance, request);
}

#if defined(_WIN32)
#define PROGRAM_NAME "claude.cmd"
#else
#define PROGRAM_NAME "claude"
#endif

static void check_platform(const cJSON *provenance) {
    char key[64];
    snprintf(key, sizeof key, "%s-%s", cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(provenance, "os")),
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(provenance, "architecture")));
    check_text(key, runtime_platform_key(), "the os and architecture");
}

static void locates_an_application_at_its_binary_on_path(void) {
    hosts_scratch place;
    fresh(&place, "provenancepath");
    char *binary = touch_file(place.path, PROGRAM_NAME, "", 0755);
    cJSON *environment = environment_of(&place);
    cJSON_AddStringToObject(environment, "PATHEXT", ".EXE;.CMD");
    catalog_answer answer = provenance_answer(
        &place, "{\"method\": \"npm\", \"package\": \"@x/claude\", \"options\": {\"global\": true}, \"version\": \"2.0.0\"}",
        environment, 1);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    check_text(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(answer.tree, "location")), binary, "the location");
    check_text(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(answer.tree, "method")), "npm", "the method");
    check_text(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(answer.tree, "package")), "@x/claude", "the package");
    check_text(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(answer.tree, "version")), "2.0.0", "the version");
    cJSON *options = parsed("{\"global\": true}");
    check_same(cJSON_GetObjectItemCaseSensitive(answer.tree, "options"), options, "the options");
    cJSON_Delete(options);
    check_text(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(answer.tree, "installedAt")),
               "2026-01-01T00:00:00.123Z", "installedAt");
    check_platform(answer.tree);
    cJSON_Delete(answer.tree);
    free(binary);
    discard(&place);
}

static void locates_an_application_off_path_at_its_home_and_leaves_out_what_its_origin_lacks(void) {
    hosts_scratch place;
    fresh(&place, "provenancehome");
    double before = runtime_now_milliseconds();
    catalog_answer answer = provenance_answer(&place, "{\"method\": \"path\"}", environment_of(&place), 0);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    check_text(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(answer.tree, "location")), place.user, "the home");
    CHECK(cJSON_GetObjectItemCaseSensitive(answer.tree, "package") == NULL);
    CHECK(cJSON_GetObjectItemCaseSensitive(answer.tree, "options") == NULL);
    CHECK(cJSON_GetObjectItemCaseSensitive(answer.tree, "version") == NULL);
    CHECK(cJSON_GetArraySize(answer.tree) == 5);
    double installed_at = 0;
    CHECK(runtime_catalog_parse_time(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(answer.tree, "installedAt")),
                                     &installed_at));
    CHECK(installed_at >= before - 1 && installed_at <= runtime_now_milliseconds() + 1);
    check_platform(answer.tree);
    cJSON_Delete(answer.tree);
    answer = provenance_answer(&place, "{\"version\": \"1.0.0\"}", environment_of(&place), 0);
    CHECK(answer.status == SPISOR_STATUS_FAILURE);
    cJSON_Delete(answer.tree);
    discard(&place);
}

#ifdef _WIN32
#define ENVIRONMENT_KEY L"Environment"
#define READ_FLAGS (RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND)

typedef struct registry_text {
    wchar_t text[8192];
    DWORD type;
    int present;
} registry_text;

static void read_value(const wchar_t *name, registry_text *value) {
    DWORD size = sizeof value->text;
    value->type = 0;
    value->text[0] = L'\0';
    value->present =
        RegGetValueW(HKEY_CURRENT_USER, ENVIRONMENT_KEY, name, READ_FLAGS, &value->type, value->text, &size) ==
        ERROR_SUCCESS;
}

static void delete_test_value(void) {
    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, ENVIRONMENT_KEY, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, RUNTIME_PATH_ENTRY_TEST_VALUE);
        RegCloseKey(key);
    }
}

/** Whether the redirected test value names nothing under this run's scratch place. */
static int test_value_names_no_scratch(void) {
    registry_text value;
    read_value(RUNTIME_PATH_ENTRY_TEST_VALUE, &value);
    return !value.present || wcsstr(value.text, L"hosts-") == NULL;
}
#endif

int main(void) {
#ifdef _WIN32
    registry_text real_before;
    read_value(L"Path", &real_before);
    delete_test_value();
#endif
    refuses_an_environment_with_no_user_home_before_registering_anything();
    refuses_an_identity_with_no_home_before_registering_anything();
    records_the_provenance_of_the_first_start();
    keeps_the_first_provenance_when_a_later_start_gives_a_later_installed_at();
    answers_the_same_command_name_on_a_second_start_and_requests_nothing_new();
    records_how_a_launcher_starts_the_host();
    refuses_a_start_with_an_unknown_runtime_or_an_entry_outside_the_bundle();
    reports_a_refused_command_for_a_host_that_can_run_without_one();
    refuses_a_host_that_requires_the_command_it_was_refused();
    removes_a_host_by_reversing_its_changes_and_then_forgetting_it();
    leaves_the_registration_and_its_references_in_place_when_the_reverse_is_refused();
    answers_as_orphans_the_hosts_whose_install_location_is_gone();
    adds_renames_and_deletes_a_command();
    deletes_the_references_file_and_leaves_another_hosts();
    deletes_every_run_record_of_the_host_with_its_references();
    deletes_the_holders_file_of_the_host();
    deletes_the_boot_hint_of_the_host_and_leaves_another_hosts();
    refuses_to_remove_a_host_id_that_is_no_plain_file_name();
    locates_an_application_at_its_binary_on_path();
    locates_an_application_off_path_at_its_home_and_leaves_out_what_its_origin_lacks();
#ifdef _WIN32
    CHECK(test_value_names_no_scratch());
    delete_test_value();
    registry_text real_after;
    read_value(L"Path", &real_after);
    CHECK(real_before.present == real_after.present && real_before.type == real_after.type &&
          wcscmp(real_before.text, real_after.text) == 0);
#endif
    return check_summary("hosts_test");
}
