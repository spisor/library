#include "home_layout.h"

#include <string.h>

#include "definition_support.h"
#include "paths.h"
#include "places.h"
#include "spisor/allocation.h"
#include "spisor/path.h"
#include "user_home.h"

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
    spisor_memory_release(layout->secrets_file);
    memset(layout, 0, sizeof *layout);
}

void runtime_testing_layout_variables(const runtime_testing_layout *layout,
                                      runtime_variable variables[RUNTIME_TESTING_VARIABLE_COUNT]) {
    const runtime_variable laid_out[RUNTIME_TESTING_VARIABLE_COUNT] = {
        {RUNTIME_PORTABLE_VARIABLE, layout->portable}, {RUNTIME_STATE_VARIABLE, layout->state},
        {RUNTIME_CACHE_VARIABLE, layout->cache},       {RUNTIME_SECRETS_VARIABLE, layout->secrets},
        {RUNTIME_USER_PROFILE_VARIABLE, layout->user}, {RUNTIME_HOME_VARIABLE, layout->user}};
    memcpy(variables, laid_out, sizeof laid_out);
}

cJSON *runtime_testing_layout_environment(const runtime_testing_layout *layout) {
    runtime_variable variables[RUNTIME_TESTING_VARIABLE_COUNT];
    cJSON *environment = NULL;
    runtime_testing_layout_variables(layout, variables);
    return runtime_environment_json(variables, RUNTIME_TESTING_VARIABLE_COUNT, &environment) == SPISOR_OK ? environment
                                                                                                         : NULL;
}

/** The secrets file runtime's places resolve once the layout's variables are set. */
static int place_secrets_file(runtime_testing_layout *layout) {
    runtime_places places = {NULL, NULL, NULL, NULL, 0};
    cJSON *environment = runtime_testing_layout_environment(layout);
    int placed = environment != NULL && runtime_resolve_places(RUNTIME_HOST_IS_WINDOWS, RUNTIME_HOST_IS_MACOS,
                                                               layout->user, environment, &places) == RUNTIME_PLACES_OK;
    cJSON_Delete(environment);
    if (placed) {
        layout->secrets_file = places.secrets_file;
        layout->force_file_store = places.force_file_store;
        places.secrets_file = NULL;
        runtime_free_places(&places);
    }
    return placed;
}

int runtime_testing_layout_of(const char *base, runtime_testing_layout *layout) {
    memset(layout, 0, sizeof *layout);
    layout->user = below(base, "user", NULL);
    layout->portable = below(base, "portable", NULL);
    layout->state = below(base, "state", NULL);
    layout->cache = below(base, "cache", NULL);
    layout->secrets = below(base, "secrets", NULL);
    layout->application = below(base, "application", NULL);
    layout->catalog = below(base, "catalog", "index.json");
    if (layout->user == NULL || layout->portable == NULL || layout->state == NULL || layout->cache == NULL ||
        layout->secrets == NULL || layout->application == NULL || layout->catalog == NULL ||
        !place_secrets_file(layout)) {
        runtime_testing_free_layout(layout);
        return 0;
    }
    return 1;
}
