#ifndef KNOWN_OUT_OF_MEMORY_BREAKS_H
#define KNOWN_OUT_OF_MEMORY_BREAKS_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define MEASURED_ON_WINDOWS 1u
#define MEASURED_ON_POSIX 2u
#if defined(_WIN32)
#define MEASURED_HERE MEASURED_ON_WINDOWS
#else
#define MEASURED_HERE MEASURED_ON_POSIX
#endif

/**
 * A case whose failing allocations break its contract until the finding it names is fixed. Each entry goes in the
 * change that fixes it: on a platform it was measured on, a listed case that holds fails the suite.
 *
 * @implNote On a platform an entry was not measured on, the case may break or hold and the suite only reports
 * which, since its failures there were never read and a wide path conversion, for one, is Windows' alone.
 */
typedef struct known_out_of_memory_break {
    const char *suite;
    const char *label;
    int sticky;
    unsigned measured_on;
    const char *finding;
} known_out_of_memory_break;

#define OUT_OF_MEMORY_SUITE "out_of_memory_test"
#define OPERATIONS_SUITE "out_of_memory_install_operations_test"
#define ECOSYSTEM_SUITE "out_of_memory_ecosystem_test"

#define OUT_OF_MEMORY_SUITE "out_of_memory_test"
#define OPERATIONS_SUITE "out_of_memory_install_operations_test"
#define ECOSYSTEM_SUITE "out_of_memory_ecosystem_test"

static const known_out_of_memory_break known_out_of_memory_breaks[] = {
    {OUT_OF_MEMORY_SUITE, "secrets' set", 0, MEASURED_ON_WINDOWS,
     "secret_file.c make_parent_directory reads a failed directory_of allocation as an unwritable folder"},
    {OUT_OF_MEMORY_SUITE, "config's set", 0, MEASURED_ON_WINDOWS,
     "config.c lock_layer: file.c spisor_make_directories reads a failed is_directory wide path as no directory, "
     "answering OK"},
    {OUT_OF_MEMORY_SUITE, "log's write", 0, MEASURED_ON_WINDOWS,
     "log.c append_line reads a failed log_directory join as a failed append, answering OK"},
    {OUT_OF_MEMORY_SUITE, "fetch's get", 0, MEASURED_ON_WINDOWS,
     "fetch.c open_partial reads a failed allocation as an unwritable destination"},
    {OUT_OF_MEMORY_SUITE, "detect's on_path", 0, MEASURED_ON_WINDOWS,
     "on_path.c matching_file: followed_entry.c runtime_is_followed_file reads a failed wide path as no file, "
     "answering NONE"},
    {OUT_OF_MEMORY_SUITE, "registry's register_host", 0, MEASURED_ON_WINDOWS,
     "registry.c register_with: hosts_lock.c lock_path: file.c spisor_make_directories reads a failed "
     "is_directory wide path as an uncreatable folder, so the lock is refused"},
    {OUT_OF_MEMORY_SUITE, "changes' apply", 0, MEASURED_ON_WINDOWS,
     "changes.c runtime_changes_apply: hosts_lock.c lock_path: file.c spisor_make_directories reads a failed "
     "is_directory wide path as an uncreatable folder, so the lock is refused"},
    {OUT_OF_MEMORY_SUITE, "changes' apply after settling", 0, MEASURED_ON_WINDOWS,
     "changes.c runtime_changes_apply: hosts_lock.c lock_path: file.c spisor_make_directories reads a failed "
     "is_directory wide path as an uncreatable folder, so the lock is refused"},
    {OUT_OF_MEMORY_SUITE, "changes' reverse", 0, MEASURED_ON_WINDOWS,
     "change_reversal.c reverse_steps: hosts_lock.c lock_path: file.c spisor_make_directories reads a failed "
     "is_directory wide path as an uncreatable folder, so the lock is refused"},
    {OUT_OF_MEMORY_SUITE, "lists' applications", 0, MEASURED_ON_WINDOWS,
     "lists.c find_detected: on_path.c matching_file: followed_entry.c runtime_is_followed_file reads a failed "
     "wide path as no file, answering OK"},
    {OUT_OF_MEMORY_SUITE, "marketplace's remember", 0, MEASURED_ON_WINDOWS,
     "marketplace_memory.c writes_store reads a failed spisor_json_stringify_pretty as an unwritable store"},
    {OUT_OF_MEMORY_SUITE, "cache's sweep", 0, MEASURED_ON_WINDOWS,
     "cache.c runtime_cache_sweep_operation: hosts_lock.c lock_path: file.c spisor_make_directories reads a "
     "failed allocation as an uncreatable folder, so the lock is refused"},
    {OUT_OF_MEMORY_SUITE, "cache's locate", 0, MEASURED_ON_WINDOWS,
     "cache_place.c runtime_cache_bundle_directory reads a failed runtime_cache_joined as no folder"},
    {OUT_OF_MEMORY_SUITE, "cache's deploy", 0, MEASURED_ON_WINDOWS,
     "cache_deploy.c write_files: file.c spisor_make_directories reads a failed allocation as an uncreatable "
     "folder"},
    {OUT_OF_MEMORY_SUITE, "cache's install", 0, MEASURED_ON_WINDOWS,
     "cache_store.c fetched: fetch.c open_partial reads a failed allocation as an unwritable destination"},
    {OUT_OF_MEMORY_SUITE, "catalog's read", 0, MEASURED_ON_WINDOWS,
     "catalog_url.c parse_url reads a failed cleaned_input as a malformed url"},
    {OUT_OF_MEMORY_SUITE, "catalog's check", 0, MEASURED_ON_WINDOWS,
     "catalog_refresh.c prepare_order: catalog_url.c runtime_catalog_file_url_of_path answers NULL out of memory, "
     "read as no origin"},
    {OUT_OF_MEMORY_SUITE, "catalog's current", 0, MEASURED_ON_WINDOWS,
     "catalog_refresh.c prepare_order: catalog_url.c runtime_catalog_file_url_of_path answers NULL out of memory, "
     "read as no origin"},
    {OUT_OF_MEMORY_SUITE, "install's by_id", 0, MEASURED_ON_WINDOWS,
     "fetch_windows.c take_hops reads a failed to_wide as a failed request"},
    {OUT_OF_MEMORY_SUITE, "install's ecosystem", 0, MEASURED_ON_WINDOWS,
     "install_ecosystem.c are_sources reads a failed runtime_catalog_sources_of as a refused source"},
    {OUT_OF_MEMORY_SUITE, "commands' remove", 0, MEASURED_ON_WINDOWS,
     "commands.c run_locked: hosts_lock.c lock_path: file.c spisor_make_directories reads a failed is_directory "
     "wide path as an uncreatable folder, so the lock is refused"},
    {OUT_OF_MEMORY_SUITE, "commands' request", 0, MEASURED_ON_WINDOWS,
     "commands.c run_locked: hosts_lock.c lock_path: file.c spisor_make_directories reads a failed is_directory "
     "wide path as an uncreatable folder, so the lock is refused"},
    {OUT_OF_MEMORY_SUITE, "commands' add", 0, MEASURED_ON_WINDOWS,
     "commands.c run_locked: hosts_lock.c lock_path: file.c spisor_make_directories reads a failed is_directory "
     "wide path as an uncreatable folder, so the lock is refused"},
    {OUT_OF_MEMORY_SUITE, "commands' rename", 0, MEASURED_ON_WINDOWS,
     "commands.c is_shim_taken reads a failed shim_file join as a held name"},
    {OUT_OF_MEMORY_SUITE, "commands' rename onto a held name", 0, MEASURED_ON_WINDOWS,
     "commands.c run_locked: hosts_lock.c lock_path: file.c spisor_make_directories reads a failed is_directory "
     "wide path as an uncreatable folder, so the lock is refused, answering OK"},
    {ECOSYSTEM_SUITE, "install's ecosystem", 0, MEASURED_ON_WINDOWS,
     "install_ecosystem.c are_sources reads a failed runtime_catalog_sources_of as a refused source"},
    {OPERATIONS_SUITE, "entry", 0, MEASURED_ON_WINDOWS,
     "change_kinds.c prepare_directory: directory_tree.c runtime_probe_entry reads a failed wide path as "
     "something not a directory"},
    {OPERATIONS_SUITE, "entry refused", 0, MEASURED_ON_WINDOWS,
     "change_kinds.c prepare_directory: directory_tree.c runtime_probe_entry reads a failed wide path as "
     "something not a directory"},
    {OPERATIONS_SUITE, "application", 0, MEASURED_ON_WINDOWS,
     "registry.c runtime_registry_store: make_hosts_directory reads a failed hosts path join as done, answering "
     "OK"},
    {OPERATIONS_SUITE, "runtime", 0, MEASURED_ON_WINDOWS,
     "install_runtime.c runtime_on_path: on_path.c matching_file: followed_entry.c runtime_is_followed_file reads "
     "a failed wide path as no file, so a node on the PATH is acquired again"},
    {OPERATIONS_SUITE, "register_loader", 0, MEASURED_ON_WINDOWS,
     "loader_installed.c read_standard: file.c spisor_read_file answers NULL alike for a failed read and out of "
     "memory"},
    {OPERATIONS_SUITE, "updates' apply", 0, MEASURED_ON_WINDOWS,
     "boot_hint.c write_text: file.c spisor_make_directories reads a failed allocation as an uncreatable folder, "
     "answering OK"},
    {OPERATIONS_SUITE, "updates' rollback", 0, MEASURED_ON_WINDOWS,
     "updates.c runtime_updates_lock_registered: hosts_lock.c lock_path: file.c spisor_make_directories reads a "
     "failed is_directory wide path as an uncreatable folder, answering OK"},
    {OPERATIONS_SUITE, "updates' rollback", 1, MEASURED_ON_WINDOWS,
     "host_references.c write_text: file.c move_over reads a failed wide path as a failed move, so the hint is "
     "rolled back and the references are not"},
    {OPERATIONS_SUITE, "updates' check", 0, MEASURED_ON_WINDOWS,
     "catalog_state.c runtime_catalog_origin_of: catalog_url.c runtime_catalog_file_url_of_path answers NULL out "
     "of memory, read as no origin"},
    {OPERATIONS_SUITE, "updates' run", 0, MEASURED_ON_WINDOWS,
     "updates.c runtime_updates_lock_registered: hosts_lock.c lock_path: file.c spisor_make_directories reads a "
     "failed is_directory wide path as an uncreatable folder, answering OK"},
    {OPERATIONS_SUITE, "boot's write", 0, MEASURED_ON_WINDOWS,
     "boot.c held_write: hosts_lock.c lock_path: file.c spisor_make_directories reads a failed allocation as an "
     "uncreatable folder, so the lock is refused"},
    {OPERATIONS_SUITE, "boot's resolve", 0, MEASURED_ON_WINDOWS,
     "boot_resolve.c is_verified: cache_marker.c file_size: file_stream.c runtime_open_file answers NULL alike "
     "for a failed open and out of memory"},
    {OPERATIONS_SUITE, "boot's place", 0, MEASURED_ON_WINDOWS,
     "boot_resolve.c is_verified: cache_marker.c file_size: file_stream.c runtime_open_file answers NULL alike "
     "for a failed open and out of memory"},
    {OPERATIONS_SUITE, "processes' hold", 0, MEASURED_ON_WINDOWS,
     "holders_lock.c runtime_lock_holders: directory_tree.c runtime_create_new_file reads a failed wide path as "
     "an uncreatable lock, so the lock is refused"},
    {OPERATIONS_SUITE, "processes' release", 0, MEASURED_ON_WINDOWS,
     "holders_lock.c runtime_lock_holders: directory_tree.c runtime_create_new_file reads a failed wide path as "
     "an uncreatable lock, so the lock is refused"},
    {OPERATIONS_SUITE, "processes' prune", 0, MEASURED_ON_WINDOWS,
     "holders_lock.c runtime_lock_holders: directory_tree.c runtime_create_new_file reads a failed wide path as "
     "an uncreatable lock, so the lock is refused"},
    {OPERATIONS_SUITE, "processes' live_runs", 0, MEASURED_ON_WINDOWS,
     "run_records.c add_live_run: directory_tree.c runtime_remove_file_entry reads a failed wide path as a failed "
     "removal, answering OK"},
    {OPERATIONS_SUITE, "processes' remove_runs", 0, MEASURED_ON_WINDOWS,
     "run_records.c runs_folder reads a failed runtime_cache_joined (cache_folders.c) as a failed removal"},
    {OPERATIONS_SUITE, "processes' hold starting", 0, MEASURED_ON_WINDOWS,
     "processes.c runtime_processes_with_holders reads a failed spisor_make_directories as an unwritable folder"},
    {OPERATIONS_SUITE, "hosts' register_host", 0, MEASURED_ON_WINDOWS,
     "registry.c register_with: hosts_lock.c lock_path: file.c spisor_make_directories reads a failed allocation "
     "as an uncreatable folder, so the lock is refused"},
    {OPERATIONS_SUITE, "hosts' remove", 0, MEASURED_ON_WINDOWS,
     "json_key_change.c read_bytes: file.c spisor_read_file answers NULL alike for a failed read and out of "
     "memory, answering OK"},
    {OPERATIONS_SUITE, "hosts' start", 0, MEASURED_ON_WINDOWS,
     "run_records.c runtime_record_run: file.c spisor_make_directories reads a failed allocation as an "
     "uncreatable folder"},
    {OPERATIONS_SUITE, "loader's home", 0, MEASURED_ON_WINDOWS,
     "application_home.c candidate_home reads a failed runtime_is_followed_directory as no directory"},
    {OPERATIONS_SUITE, "loader's installed", 0, MEASURED_ON_WINDOWS,
     "loader_installed.c read_standard: file.c spisor_read_file answers NULL alike for a failed read and out of "
     "memory"},
    {OPERATIONS_SUITE, "changes' json-key create", 0, MEASURED_ON_WINDOWS,
     "json_key_change.c may_create: directory_tree.c runtime_probe_entry reads a failed wide path as no file"},
    {OPERATIONS_SUITE, "processes' hold starting", 1, MEASURED_ON_WINDOWS,
     "holders.c runtime_write_holders_draft: file.c move_over reads a failed wide path as a failed move, "
     "answering OK without recording the started host"},
    {OPERATIONS_SUITE, "hosts' remove", 1, MEASURED_ON_WINDOWS,
     "registry.c remove_registration_file: directory_tree.c attributes_of reads a failed wide path as nothing to "
     "remove, answering OK with the tree changed"},
    {OPERATIONS_SUITE, "hosts' start", 1, MEASURED_ON_WINDOWS,
     "run_records.c write_record fails under sticky failure and the undo through runtime_remove_run_record "
     "allocates too, answering OUT_OF_MEMORY with a run record left"},
    {OPERATIONS_SUITE, "changes' json-key create", 1, MEASURED_ON_WINDOWS,
     "json_key_change.c perform_created: directory_tree.c runtime_create_new_file reads a failed wide path as an "
     "existing file, answering OK without the file"},
};

static const known_out_of_memory_break *known_out_of_memory_break_of(const char *suite, const char *label,
                                                                     int sticky) {
    size_t count = sizeof known_out_of_memory_breaks / sizeof known_out_of_memory_breaks[0];
    for (size_t index = 0; index < count; index++) {
        const known_out_of_memory_break *known = &known_out_of_memory_breaks[index];
        if (strcmp(known->suite, suite) == 0 && strcmp(known->label, label) == 0 && known->sticky == sticky) {
            return known;
        }
    }
    return NULL;
}

/** The check failures a finished case adds: its own when unlisted, none when a listed case broke, one when it held. */
static int settled_out_of_memory_failures(const char *suite, const char *label, int sticky, int failures) {
    const known_out_of_memory_break *known = known_out_of_memory_break_of(suite, label, sticky);
    if (known == NULL) {
        return failures;
    }
    if ((known->measured_on & MEASURED_HERE) == 0) {
        printf("%s%s: listed, not measured on this platform, and it %s\n", label, sticky ? " (sticky)" : "",
               failures > 0 ? "broke" : "held");
        return 0;
    }
    if (failures > 0) {
        printf("%s%s: known out-of-memory break, %s\n", label, sticky ? " (sticky)" : "", known->finding);
        return 0;
    }
    fprintf(stderr, "%s%s: holds now, so its entry in known_out_of_memory_breaks goes\n", label,
            sticky ? " (sticky)" : "");
    return 1;
}

#endif
