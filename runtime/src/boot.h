#ifndef RUNTIME_BOOT_H
#define RUNTIME_BOOT_H

#include <stddef.h>
#include <stdint.h>

#include "export.h"

/** boot.read: {cache, hostId} answers {hint?: {libraries, host?}}, no hint when none is readable. */
RUNTIME_EXPORT int32_t runtime_boot_read(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/**
 * boot.write: {cache, hostId, libraries, host?, state?} records the libraries and host bundle version the
 * next start of hostId boots, keeping the recorded host version when host is left out, and answers {hint}.
 *
 * @implNote Keeping the recorded host is a read-modify-write, so with state it holds the registry lock
 * under it, the one updates holds while it switches a hint, and a write never reverts a switch.
 */
RUNTIME_EXPORT int32_t runtime_boot_write(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/**
 * boot.resolve: {cache, hostId, arguments?, platform?} answers {executable, arguments, version, directory}:
 * the native host executable of the version hostId's hint names,
 * cache/bundles/<hostId>/<version>/<platform>/<hostId>, with .exe on a win32 platform, and the
 * arguments to run it with, those given passed through.
 *
 * @implNote A hint that is missing or unreadable, a version that is not installed, or one whose files no
 * longer carry their recorded hashes, is refused with the reinstall fix: no version is ever guessed, so
 * a command never runs something other than what install and updates chose.
 */
RUNTIME_EXPORT int32_t runtime_boot_resolve(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/**
 * boot.place: {cache} copies the forebay-boot the hinted forebay-host version carries to cache/boot when
 * none is there yet, made runnable, and answers {placed, path}; placed is false when one was already there
 * or no hinted version carries one. A copy that cannot be written is refused.
 *
 * @implNote Never replaces a stub that is there: forebay-boot runs every command, and how a running one is
 * replaced is the launcher update's to decide.
 */
RUNTIME_EXPORT int32_t runtime_boot_place(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
