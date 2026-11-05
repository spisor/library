#include "updates_fixture.h"

#define LOADER "fixture-loader"

static cJSON *no_settings(void) {
    return settings_with("{}");
}

static catalog_answer apply_plain(const char *host, const char *settings_json) {
    return apply_with(host, settings_with(settings_json), "{}");
}

static catalog_answer remove_host(const char *host) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "places", places_json());
    cJSON_AddStringToObject(argument, "id", host);
    return call_catalog(runtime_hosts_remove, argument);
}

static void expect_nothing_recorded_for(const char *host, const char *label) {
    char references[512];
    char runs[512];
    char hint[512];
    snprintf(references, sizeof references, "bundles/%s.json", host);
    snprintf(runs, sizeof runs, "bundles/runs/%s", host);
    snprintf(hint, sizeof hint, "hosts/%s.json", host);
    if (exists_in(state, references) || exists_in(state, runs) || exists_in(cache, hint)) {
        fprintf(stderr, "%s: references %d, run records %d, boot hint %d\n", label, exists_in(state, references),
                exists_in(state, runs), exists_in(cache, hint));
        check_failures++;
    }
}

static void it_records_nothing_for_a_removed_host_and_refuses_its_updates(void) {
    start_base("removed-host");
    list_plugin("a", "1.0.0", "1.0.0");
    write_catalog();
    mark_installed("bundles", "a", "1.0.0");
    catalog_answer started = apply_plain(LOADER, "{\"plugins\":{\"a\":{\"enabled\":true}}}");
    expect_success(started, "start before removal");
    write_hint(LOADER, "{\"libraries\":{},\"host\":\"1.0.0\"}");
    catalog_answer removed = remove_host(LOADER);
    expect_success(removed, "removal");
    expect_nothing_recorded_for(LOADER, "after removal");

    catalog_answer applied =
        apply_unregistered(LOADER, settings_with("{\"plugins\":{\"a\":{\"enabled\":true}}}"), "{}");
    expect_refusal(applied, "is not registered", "apply after removal");
    catalog_answer checked = check_unregistered(LOADER, no_settings(), "{}");
    expect_refusal(checked, "is not registered", "check after removal");
    expect_nothing_recorded_for(LOADER, "after its updates");
    cJSON_Delete(started.tree);
    cJSON_Delete(removed.tree);
    cJSON_Delete(applied.tree);
    cJSON_Delete(checked.tree);
    finish_base();
}

int main(void) {
    it_records_nothing_for_a_removed_host_and_refuses_its_updates();
    return check_summary("host_removal_test");
}
