#ifndef HOST_BOOT_PLACEMENT_H
#define HOST_BOOT_PLACEMENT_H

#include "cJSON.h"
#include "host_context.h"

/**
 * Puts forebay-boot at cache/boot/forebay-boot, with .exe on Windows, copied from the forebay-host version
 * the boot hint names when that build carries one beside its executable and none is in place yet; NULL
 * when done or there is nothing to do, else the refusal.
 *
 * @implNote One already in place is never replaced here: every command shim runs it, so on Windows it may
 * be running this very command, and replacing a running forebay-boot is the launcher's own update.
 */
cJSON *host_place_boot_stub(const host_context *context);

#endif
