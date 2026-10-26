#ifndef SPISOR_PATH_H
#define SPISOR_PATH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Node's path.win32.join when windows is non-zero, path.posix.join otherwise. The result is
 * freed with free, and is NULL only when memory runs out.
 *
 * @implNote Ported from Node 26, including its handling of reserved device names such as CON,
 * which makes join("CONx") answer ".\\CONx" on Windows. Pinned by test/vectors/path-join.json.
 */
char *spisor_path_join(int windows, const char *const *segments, size_t count);

int spisor_path_is_absolute(int windows, const char *path);

/** basekit's expandHome: "~", "~/rest" and "~\\rest" resolve against home; anything else is copied. */
char *spisor_expand_home(int windows, const char *path, const char *home);

#ifdef __cplusplus
}
#endif

#endif
