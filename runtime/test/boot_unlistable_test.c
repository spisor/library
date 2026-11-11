#include "updates_fixture.h"

#include "cache_place.h"

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#define HOST "spisor-host"
#define SKIPPED 77

#if !defined(_WIN32)
static char *library_folder(const char *id, const char *version) {
    const char *segments[4] = {"libraries", id, version, PLATFORM};
    return runtime_cache_resolve(cache, segments, 4);
}

/** A declared library folder of id at version that this process can look inside but not list. */
static char *unlistable_library(const char *id, const char *version) {
    char *folder = library_folder(id, version);
    write_in(folder, "library.json", "{}");
    write_in(folder, ".installed.json", "{\"files\":{}}");
    write_in(folder, "libruntime.so", "binary");
    CHECK(chmod(folder, 0300) == 0);
    return folder;
}

static void make_listable(const char *folder) {
    CHECK(chmod(folder, 0700) == 0);
}

static void expect_could_not_list(const char *text, const char *folder, const char *label) {
    char expected[2048];
    snprintf(expected, sizeof expected, "could not list %s", folder);
    if (text == NULL || strstr(text, expected) == NULL) {
        fprintf(stderr, "%s: expected \"%s\" in %s\n", label, expected, text == NULL ? "(none)" : text);
        check_failures++;
    }
}

static void a_boot_write_refuses_a_library_folder_it_cannot_list(void) {
    start_base("unlistable-write");
    char *folder = unlistable_library("runtime", "0.1.0");
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "cache", cache);
    cJSON_AddStringToObject(argument, "hostId", HOST);
    cJSON_AddItemToObject(argument, "libraries", parsed("{\"runtime\":\"0.1.0\"}"));
    cJSON_AddStringToObject(argument, "platform", PLATFORM);
    catalog_answer written = call_catalog(runtime_boot_write, argument);
    CHECK(written.status == SPISOR_STATUS_FAILURE);
    expect_could_not_list(text_at(written.tree, "message"), folder, "boot.write");
    CHECK(!exists_in(cache, "hosts/" HOST ".json"));
    make_listable(folder);
    cJSON_Delete(written.tree);
    free(folder);
    finish_base();
}

static void an_apply_reports_a_library_folder_it_cannot_list_as_unrecorded(void) {
    start_base("unlistable-apply");
    list_library("runtime", "0.1.0", "0.1.0");
    write_catalog();
    char *folder = unlistable_library("runtime", "0.1.0");
    catalog_answer started = apply_with(HOST, settings_with("{}"),
                                        "{\"running\":{\"" HOST "\":\"1.0.0\",\"runtime\":\"0.1.0\"},"
                                        "\"libraries\":[\"runtime\"]}");
    expect_success(started, "apply");
    const cJSON *unrecorded = cJSON_GetArrayItem(value_at(started.tree, "unrecorded"), 0);
    expect_could_not_list(text_at(unrecorded, "message"), folder, "apply unrecorded");
    make_listable(folder);
    cJSON_Delete(started.tree);
    free(folder);
    finish_base();
}
#endif

int main(void) {
#if defined(_WIN32)
    fprintf(stderr, "boot_unlistable_test: Windows has no folder that can be entered but not listed\n");
    return SKIPPED;
#else
    if (geteuid() == 0) {
        fprintf(stderr, "boot_unlistable_test: root lists every folder\n");
        return SKIPPED;
    }
    a_boot_write_refuses_a_library_folder_it_cannot_list();
    an_apply_reports_a_library_folder_it_cannot_list_as_unrecorded();
    return check_summary("boot_unlistable_test");
#endif
}
