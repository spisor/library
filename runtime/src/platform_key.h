#ifndef RUNTIME_PLATFORM_KEY_H
#define RUNTIME_PLATFORM_KEY_H

/** This build's platform key as Node spells it, `${process.platform}-${process.arch}`, such as linux-x64. */
const char *runtime_platform_key(void);

/** Whether key has the <os>-<arch> shape a platform key takes: lowercase letters and digits either side of one hyphen. */
int runtime_is_platform_key(const char *key);

#endif
