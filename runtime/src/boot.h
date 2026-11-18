#ifndef RUNTIME_BOOT_H
#define RUNTIME_BOOT_H

#include <stddef.h>

#include "cJSON.h"

/**
 * @file
 * The boot service's functions, for runtime's own callers: each answers its result, or NULL with *refusal set to
 * {message, fix}, or with it NULL when memory runs out.
 */

/** boot's read: the hint of host_id under cache as {hint?: {libraries, host?}}, no hint when none is readable. */
cJSON *runtime_boot_read(const char *cache, const char *host_id, cJSON **refusal);

/**
 * What boot's write records: the libraries, an object of library id to version, and the host bundle version, the
 * recorded one kept for a NULL host, that the next start of host_id boots on platform, this machine's for NULL.
 * A state that is not NULL names the registry lock the write holds.
 */
typedef struct runtime_boot_hint_request {
    const char *cache;
    const char *host_id;
    const cJSON *libraries;
    const char *host;
    const char *state;
    const char *platform;
} runtime_boot_hint_request;

/**
 * boot's write: records the hint the request describes; 1, or 0 with *refusal set, or with it NULL when memory runs
 * out.
 *
 * @implNote Keeping the recorded host is a read-modify-write, so with state it holds the registry lock
 * under it, the one updates holds while it switches a hint, and a write never reverts a switch.
 */
int runtime_boot_write(const runtime_boot_hint_request *request, cJSON **refusal);

/** A boot write prepared and holding its lock, not yet written. */
typedef struct runtime_boot_hint_write runtime_boot_hint_write;

/**
 * boot's write up to its one file write: the call checked, the libraries resolved, the lock taken and the answer
 * built, so a caller can take what it answers before anything is written. NULL with *refusal set, or with it NULL
 * when memory runs out.
 */
runtime_boot_hint_write *runtime_boot_prepare_write(const runtime_boot_hint_request *request, cJSON **refusal);

/** The {hint} a prepared write answers once committed, borrowed from it. */
const cJSON *runtime_boot_write_answer(const runtime_boot_hint_write *write);

/** Writes the hint; 1, or 0 with *refusal set, or with it NULL when memory runs out. */
int runtime_boot_commit_write(runtime_boot_hint_write *write, cJSON **refusal);

/** Releases the lock and what a prepared write holds. NULL does nothing. */
void runtime_boot_free_write(runtime_boot_hint_write *write);

/**
 * boot's resolve: {executable, arguments, version, directory}, the native host executable of the version host_id's
 * hint names, cache/bundles/<hostId>/<version>/<platform>/<hostId>, with .exe on a win32 platform, and the
 * arguments to run it with, those given passed through; platform is this machine's for NULL.
 *
 * @implNote A hint that is missing or unreadable, a version that is not installed, or one whose files no
 * longer carry their recorded hashes, is refused with the reinstall fix: no version is ever guessed, so
 * a command never runs something other than what install and updates chose.
 */
cJSON *runtime_boot_resolve(const char *cache, const char *host_id, const char *const *arguments,
                            size_t argument_count, const char *platform, cJSON **refusal);

/**
 * boot's resolve for this machine's platform, with no arguments to pass: {executable, version, directory}, or NULL
 * with *refusal set as resolve refuses, NULL when memory runs out. spisor-boot calls this rather than the definition,
 * so it links only what a resolve needs.
 */
cJSON *runtime_boot_resolve_host(const char *cache, const char *host_id, cJSON **refusal);

/**
 * boot's place: copies the spisor-boot the hinted spisor-host version carries to cache/boot when
 * none is there yet, made runnable, and answers {placed, path}; placed is false when one was already there
 * or no hinted version carries one. A copy that cannot be written is refused.
 *
 * @implNote Never replaces a stub that is there: spisor-boot runs every command, and how a running one is
 * replaced is the launcher update's to decide.
 */
cJSON *runtime_boot_place(const char *cache, cJSON **refusal);

#endif
