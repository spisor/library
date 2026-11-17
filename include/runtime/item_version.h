#ifndef RUNTIME_PUBLIC_ITEM_VERSION_H
#define RUNTIME_PUBLIC_ITEM_VERSION_H

/**
 * @file
 * An item id and a version of it, as runtime's services name what is installed, scheduled, booted or run.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** An item id and a version of it, both NUL-terminated UTF-8. An element of an array, so it has no size. */
typedef struct runtime_item_version {
    const char *id;
    const char *version;
} runtime_item_version;

#ifdef __cplusplus
}
#endif

#endif
