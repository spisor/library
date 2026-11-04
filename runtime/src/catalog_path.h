#ifndef RUNTIME_CATALOG_PATH_H
#define RUNTIME_CATALOG_PATH_H

/**
 * Node's path.resolve(path) for this platform: absolute against the working directory, normalized, with
 * no trailing separator but a root's. Freed with free; NULL when the working directory cannot be read or
 * memory runs out.
 *
 * @implNote A drive-relative Windows path such as C:x resolves against that drive's root, where Node would
 * take the drive's own working directory from the environment.
 */
char *runtime_catalog_resolve_path(const char *path);

#endif
