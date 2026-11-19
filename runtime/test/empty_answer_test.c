#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "check.h"
#include "runtime/boot.h"
#include "runtime/cache.h"
#include "runtime/catalog.h"
#include "runtime/changes.h"
#include "runtime/hosts.h"
#include "runtime/identity.h"
#include "runtime/install.h"
#include "runtime/lists.h"
#include "runtime/loader.h"
#include "runtime/marketplace.h"
#include "runtime/processes.h"
#include "runtime/registry.h"
#include "runtime/ui.h"
#include "runtime/updates.h"
#include "spisor/status.h"

/**
 * @file
 * Calls every operation whose answer holds arrays, through the built shared runtime as a consumer would, with an
 * argument of its first shape and an answer whose storage is NULL but one count is not zero. Each must refuse it
 * MALFORMED and leave the answer as it was: a fill appends at its count, so going on would write past the array.
 */

#ifdef _WIN32
typedef HMODULE library_handle;

static library_handle load_runtime(void) {
    return LoadLibraryA(RUNTIME_LIBRARY_PATH);
}

static const void *definition_of(library_handle library, const char *symbol) {
    FARPROC getter = GetProcAddress(library, symbol);
    return getter == NULL ? NULL : ((const void *(*)(void))(void (*)(void))getter)();
}

static void unload_runtime(library_handle library) {
    FreeLibrary(library);
}
#else
typedef void *library_handle;

static library_handle load_runtime(void) {
    return dlopen(RUNTIME_LIBRARY_PATH, RTLD_NOW | RTLD_LOCAL);
}

static const void *definition_of(library_handle library, const char *symbol) {
    void *getter = dlsym(library, symbol);
    const void *(*call)(void) = NULL;
    if (getter == NULL) {
        return NULL;
    }
    memcpy(&call, &getter, sizeof call);
    return call();
}

static void unload_runtime(library_handle library) {
    dlclose(library);
}
#endif

/** Whether a call refused a counted answer as not empty, before writing anything into it. */
static void check_refused_untouched(spisor_status status, const spisor_refusal *refusal, size_t count,
                                    const void *storage, const char *label) {
    int refused = status == SPISOR_MALFORMED && refusal->detail != NULL && strstr(refusal->detail, "not empty") != NULL;
    if (!refused || count != 3 || storage != NULL) {
        fprintf(stderr, "%s: expected MALFORMED naming a non-empty answer, untouched; got %d, \"%s\", count %zu\n",
                label, (int)status, refusal->detail == NULL ? "(none)" : refusal->detail, count);
        check_failures++;
    }
}

/** Calls `operation` of `definition` with a runtime_<argument> and a runtime_<answer> holding a nonzero `count`. */
#define EXPECT_COUNT_REFUSED(definition, operation, argument, answer_type, count)                   \
    do {                                                                                             \
        runtime_##argument sized = SPISOR_INIT(runtime_##argument);                                  \
        runtime_##answer_type answer = SPISOR_INIT(runtime_##answer_type);                           \
        spisor_refusal refusal = SPISOR_INIT(spisor_refusal);                                        \
        spisor_status status;                                                                        \
        answer.count = 3;                                                                            \
        status = (definition)->operation((definition)->context, &sized, &answer, &refusal);          \
        check_refused_untouched(status, &refusal, answer.count, answer.storage, #operation " " #count); \
        (definition)->release_refusal((definition)->context, &refusal);                              \
    } while (0)

static void boot_refuses_a_counted_answer(library_handle library) {
    const runtime_boot_definition *boot = definition_of(library, "runtime_boot_definition_of");
    CHECK(boot != NULL);
    if (boot == NULL) {
        return;
    }
    EXPECT_COUNT_REFUSED(boot, read, boot_host_argument, boot_hint_answer, library_count);
    EXPECT_COUNT_REFUSED(boot, write, boot_write_argument, boot_hint_answer, library_count);
    EXPECT_COUNT_REFUSED(boot, resolve, boot_resolve_argument, boot_resolve_answer, argument_count);
}

static void cache_refuses_a_counted_answer(library_handle library) {
    const runtime_cache_definition *cache = definition_of(library, "runtime_cache_definition_of");
    CHECK(cache != NULL);
    if (cache == NULL) {
        return;
    }
    EXPECT_COUNT_REFUSED(cache, install, cache_install_argument, cache_install_answer, fetched_count);
    EXPECT_COUNT_REFUSED(cache, install, cache_install_argument, cache_install_answer, cached_count);
    EXPECT_COUNT_REFUSED(cache, installed, cache_place_argument, cache_installed_answer, bundle_count);
    EXPECT_COUNT_REFUSED(cache, installed, cache_place_argument, cache_installed_answer, library_count);
    EXPECT_COUNT_REFUSED(cache, damaged, cache_place_argument, cache_damaged_answer, damaged_count);
    EXPECT_COUNT_REFUSED(cache, locate, cache_locate_argument, cache_locate_answer, located_count);
    EXPECT_COUNT_REFUSED(cache, locate, cache_locate_argument, cache_locate_answer, problem_count);
    EXPECT_COUNT_REFUSED(cache, sweep, cache_sweep_argument, cache_sweep_answer, removed_count);
}

static void catalog_refuses_a_counted_answer(library_handle library) {
    const runtime_catalog_definition *catalog = definition_of(library, "runtime_catalog_definition_of");
    CHECK(catalog != NULL);
    if (catalog == NULL) {
        return;
    }
    EXPECT_COUNT_REFUSED(catalog, sources, catalog_sources_argument, catalog_sources_answer, source_count);
    EXPECT_COUNT_REFUSED(catalog, sources, catalog_sources_argument, catalog_sources_answer, skipped_count);
    EXPECT_COUNT_REFUSED(catalog, merge, catalog_merge_argument, catalog_merge_answer, shadowed_count);
    EXPECT_COUNT_REFUSED(catalog, resolve, catalog_resolve_argument, catalog_resolve_answer, run_count);
    EXPECT_COUNT_REFUSED(catalog, resolve, catalog_resolve_argument, catalog_resolve_answer, fetch_count);
}

static void changes_refuses_a_counted_answer(library_handle library) {
    const runtime_changes_definition *changes = definition_of(library, "runtime_changes_definition_of");
    CHECK(changes != NULL);
    if (changes == NULL) {
        return;
    }
    EXPECT_COUNT_REFUSED(changes, reverse, changes_reverse_argument, changes_reverse_answer, drifted_count);
}

static void hosts_refuses_a_counted_answer(library_handle library) {
    const runtime_hosts_definition *hosts = definition_of(library, "runtime_hosts_definition_of");
    CHECK(hosts != NULL);
    if (hosts == NULL) {
        return;
    }
    EXPECT_COUNT_REFUSED(hosts, remove, registry_forget_argument, hosts_remove_answer, drifted_count);
}

static void identity_refuses_a_counted_answer(library_handle library) {
    const runtime_identity_definition *identity = definition_of(library, "runtime_identity_definition_of");
    CHECK(identity != NULL);
    if (identity == NULL) {
        return;
    }
    EXPECT_COUNT_REFUSED(identity, agree, identity_agree_argument, identity_agreement, id_count);
    EXPECT_COUNT_REFUSED(identity, also_at, identity_also_at_argument, identity_agreement, id_count);
}

static void install_refuses_a_counted_answer(library_handle library) {
    const runtime_install_definition *install = definition_of(library, "runtime_install_definition_of");
    CHECK(install != NULL);
    if (install == NULL) {
        return;
    }
    EXPECT_COUNT_REFUSED(install, warnings, install_warnings_argument, install_warnings_answer, warning_count);
    EXPECT_COUNT_REFUSED(install, packages, install_packages_argument, install_packages_answer, package_count);
    EXPECT_COUNT_REFUSED(install, plan, install_plan_argument, install_plan_answer, step_count);
    EXPECT_COUNT_REFUSED(install, detect, install_detect_argument, install_detect_answer, detection_count);
    EXPECT_COUNT_REFUSED(install, entry, install_entry_argument, install_entry_answer, sequence_count);
    EXPECT_COUNT_REFUSED(install, register_loader, install_register_loader_argument, install_register_loader_answer,
                         command_count);
    EXPECT_COUNT_REFUSED(install, register_loader, install_register_loader_argument, install_register_loader_answer,
                         refused_command_count);
    EXPECT_COUNT_REFUSED(install, by_id, install_by_id_argument, install_by_id_answer, id_count);
    EXPECT_COUNT_REFUSED(install, ecosystem, install_ecosystem_argument, install_ecosystem_answer, installed_count);
    EXPECT_COUNT_REFUSED(install, ecosystem, install_ecosystem_argument, install_ecosystem_answer, cached_count);
    EXPECT_COUNT_REFUSED(install, ecosystem, install_ecosystem_argument, install_ecosystem_answer, unavailable_count);
    EXPECT_COUNT_REFUSED(install, ecosystem, install_ecosystem_argument, install_ecosystem_answer, enabled_count);
    EXPECT_COUNT_REFUSED(install, ecosystem, install_ecosystem_argument, install_ecosystem_answer, application_count);
    EXPECT_COUNT_REFUSED(install, ecosystem, install_ecosystem_argument, install_ecosystem_answer, loader_count);
    EXPECT_COUNT_REFUSED(install, ecosystem, install_ecosystem_argument, install_ecosystem_answer, missing_count);
}

static void lists_refuses_a_counted_answer(library_handle library) {
    const runtime_lists_definition *lists = definition_of(library, "runtime_lists_definition_of");
    CHECK(lists != NULL);
    if (lists == NULL) {
        return;
    }
    EXPECT_COUNT_REFUSED(lists, plugins, lists_plugins_argument, list, item_count);
    EXPECT_COUNT_REFUSED(lists, applications, lists_applications_argument, list, item_count);
}

static void loader_refuses_a_counted_answer(library_handle library) {
    const runtime_loader_definition *loader = definition_of(library, "runtime_loader_definition_of");
    CHECK(loader != NULL);
    if (loader == NULL) {
        return;
    }
    EXPECT_COUNT_REFUSED(loader, check, loader_check_argument, loader_check_answer, issue_count);
}

static void marketplace_refuses_a_counted_answer(library_handle library) {
    const runtime_marketplace_definition *marketplace = definition_of(library, "runtime_marketplace_definition_of");
    CHECK(marketplace != NULL);
    if (marketplace == NULL) {
        return;
    }
    EXPECT_COUNT_REFUSED(marketplace, build, marketplace_build_argument, marketplace_build_answer, skipped_count);
    EXPECT_COUNT_REFUSED(marketplace, sources, marketplace_sources_argument, marketplace_sources_answer, source_count);
    EXPECT_COUNT_REFUSED(marketplace, sources, marketplace_sources_argument, marketplace_sources_answer, skipped_count);
    EXPECT_COUNT_REFUSED(marketplace, lookup, marketplace_lookup_argument, marketplace_lookup_answer, ambiguous_count);
}

static void processes_refuses_a_counted_answer(library_handle library) {
    const runtime_processes_definition *processes = definition_of(library, "runtime_processes_definition_of");
    CHECK(processes != NULL);
    if (processes == NULL) {
        return;
    }
    EXPECT_COUNT_REFUSED(processes, release, processes_holder_argument, processes_release_answer, holder_count);
    EXPECT_COUNT_REFUSED(processes, await_ready, processes_await_argument, processes_await_answer, environment_count);
    EXPECT_COUNT_REFUSED(processes, live_runs, processes_live_argument, processes_live_answer, run_count);
}

static void registry_refuses_a_counted_answer(library_handle library) {
    const runtime_registry_definition *registry = definition_of(library, "runtime_registry_definition_of");
    CHECK(registry != NULL);
    if (registry == NULL) {
        return;
    }
    EXPECT_COUNT_REFUSED(registry, list, registry_list_argument, registry_listing, host_count);
    EXPECT_COUNT_REFUSED(registry, list, registry_list_argument, registry_listing, unreadable_count);
}

static void ui_refuses_a_counted_answer(library_handle library) {
    const runtime_ui_definition *ui = definition_of(library, "runtime_ui_definition_of");
    CHECK(ui != NULL);
    if (ui == NULL) {
        return;
    }
    EXPECT_COUNT_REFUSED(ui, navigation, ui_navigation_argument, ui_navigation_answer, entry_count);
    EXPECT_COUNT_REFUSED(ui, settings_view, ui_settings_view_argument, ui_settings_view_answer, node_count);
    EXPECT_COUNT_REFUSED(ui, plan, ui_screen_argument, ui_plan_answer, call_count);
    EXPECT_COUNT_REFUSED(ui, render, ui_screen_argument, ui_screen, node_count);
    EXPECT_COUNT_REFUSED(ui, plugin_list, ui_plugin_list_argument, ui_screen, node_count);
}

static void updates_refuses_a_counted_answer(library_handle library) {
    const runtime_updates_definition *updates = definition_of(library, "runtime_updates_definition_of");
    CHECK(updates != NULL);
    if (updates == NULL) {
        return;
    }
    EXPECT_COUNT_REFUSED(updates, apply, updates_apply_argument, updates_apply_answer, plugin_count);
    EXPECT_COUNT_REFUSED(updates, apply, updates_apply_argument, updates_apply_answer, run_count);
    EXPECT_COUNT_REFUSED(updates, apply, updates_apply_argument, updates_apply_answer, next_count);
    EXPECT_COUNT_REFUSED(updates, apply, updates_apply_argument, updates_apply_answer, rollback_count);
    EXPECT_COUNT_REFUSED(updates, apply, updates_apply_argument, updates_apply_answer, unrecorded_count);
    EXPECT_COUNT_REFUSED(updates, check, updates_check_argument, updates_check_answer, source_count);
    EXPECT_COUNT_REFUSED(updates, check, updates_check_argument, updates_check_answer, fetched_count);
    EXPECT_COUNT_REFUSED(updates, check, updates_check_argument, updates_check_answer, refused_count);
    EXPECT_COUNT_REFUSED(updates, check, updates_check_argument, updates_check_answer, removed_count);
    EXPECT_COUNT_REFUSED(updates, rollback, updates_rollback_argument, updates_rollback_answer, run_count);
    EXPECT_COUNT_REFUSED(updates, rollback, updates_rollback_argument, updates_rollback_answer, next_count);
    EXPECT_COUNT_REFUSED(updates, rollback, updates_rollback_argument, updates_rollback_answer, rollback_count);
    EXPECT_COUNT_REFUSED(updates, recorded, updates_recorded_argument, updates_recorded_answer, run_count);
    EXPECT_COUNT_REFUSED(updates, run, updates_run_argument, updates_run_answer, check_count);
    EXPECT_COUNT_REFUSED(updates, run, updates_run_argument, updates_run_answer, application_count);
    EXPECT_COUNT_REFUSED(updates, run, updates_run_argument, updates_run_answer, refused_count);
}

int main(void) {
    library_handle library = load_runtime();
    CHECK(library != NULL);
    if (library == NULL) {
        fprintf(stderr, "could not load %s\n", RUNTIME_LIBRARY_PATH);
        return check_summary("empty_answer_test");
    }
    boot_refuses_a_counted_answer(library);
    cache_refuses_a_counted_answer(library);
    catalog_refuses_a_counted_answer(library);
    changes_refuses_a_counted_answer(library);
    hosts_refuses_a_counted_answer(library);
    identity_refuses_a_counted_answer(library);
    install_refuses_a_counted_answer(library);
    lists_refuses_a_counted_answer(library);
    loader_refuses_a_counted_answer(library);
    marketplace_refuses_a_counted_answer(library);
    processes_refuses_a_counted_answer(library);
    registry_refuses_a_counted_answer(library);
    ui_refuses_a_counted_answer(library);
    updates_refuses_a_counted_answer(library);
    unload_runtime(library);
    return check_summary("empty_answer_test");
}
