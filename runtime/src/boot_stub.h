#ifndef RUNTIME_BOOT_STUB_H
#define RUNTIME_BOOT_STUB_H

/** The host id forebay-boot runs for every command a loader holds and every entry an application starts. */
#define RUNTIME_BOOTED_HOST_ID "forebay-host"

/**
 * cache/boot/forebay-boot, with .exe on Windows, freed with free; NULL when memory runs out.
 *
 * @implNote The one place the versionless stub lives, which every command shim and application entry
 * runs as forebay-boot <host-id> <arguments>; the stub reads the host's boot hint, so no target names a
 * version a sweep can delete.
 */
char *runtime_boot_stub_path(const char *cache);

#endif
