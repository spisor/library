#include "home_layout.h"

#include <string.h>

#include "spisor/allocation.h"
#include "paths.h"
#include "spisor/path.h"

static char *below(const char *base, const char *first, const char *second) {
    const char *segments[3] = {base, first, second};
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, second == NULL ? 2 : 3);
}

void runtime_testing_free_layout(runtime_testing_layout *layout) {
    spisor_memory_release(layout->user);
    spisor_memory_release(layout->portable);
    spisor_memory_release(layout->state);
    spisor_memory_release(layout->cache);
    spisor_memory_release(layout->secrets);
    spisor_memory_release(layout->application);
    spisor_memory_release(layout->catalog);
    spisor_memory_release(layout->koffi);
    memset(layout, 0, sizeof *layout);
}

int runtime_testing_layout_of(const char *base, runtime_testing_layout *layout) {
    layout->user = below(base, "user", NULL);
    layout->portable = below(base, "portable", NULL);
    layout->state = below(base, "state", NULL);
    layout->cache = below(base, "cache", NULL);
    layout->secrets = below(base, "secrets", NULL);
    layout->application = below(base, "application", NULL);
    layout->catalog = below(base, "catalog", "index.json");
    layout->koffi = below(base, "koffi", NULL);
    if (layout->user == NULL || layout->portable == NULL || layout->state == NULL || layout->cache == NULL ||
        layout->secrets == NULL || layout->application == NULL || layout->catalog == NULL || layout->koffi == NULL) {
        runtime_testing_free_layout(layout);
        return 0;
    }
    return 1;
}
