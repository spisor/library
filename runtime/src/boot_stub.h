#ifndef RUNTIME_BOOT_STUB_H
#define RUNTIME_BOOT_STUB_H

#if defined(_WIN32)
#define RUNTIME_BOOT_STUB_NAME "spisor-boot.exe"
#else
#define RUNTIME_BOOT_STUB_NAME "spisor-boot"
#endif

/**
 * cache/boot/spisor-boot, with .exe on Windows, freed with free; NULL when memory runs out.
 *
 * @implNote The one place the versionless stub lives, which every command shim and application entry
 * runs as spisor-boot <host-id> <arguments>; the stub reads the host's boot hint, so no target names a
 * version a sweep can delete.
 */
char *runtime_boot_stub_path(const char *cache);

#endif
