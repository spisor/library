#include <stdlib.h>

#include "cache_refusal.h"
#include "catalog_answer.h"
#include "host_references.h"
#include "node_json_file.h"
#include "spisor/file.h"
#include "updates_request.h"

static cJSON *unreadable(const runtime_updates_request *request, const char *path) {
    const char *parts[5] = {"the references of ", request->host_id, " in ", path,
                            " could not be read, so what its last start ran is unknown"};
    const char *fix_parts[4] = {"delete ", path, " so the next start of ", request->host_id};
    char *fix_start = runtime_cache_text(fix_parts, 4);
    const char *fix_whole[2] = {fix_start, " records it again"};
    char *fix = fix_start == NULL ? NULL : runtime_cache_text(fix_whole, 2);
    cJSON *refusal = fix == NULL ? NULL : runtime_cache_refusal(parts, 5, fix);
    free(fix);
    free(fix_start);
    return refusal;
}

static cJSON *run_answer(const cJSON *run) {
    cJSON *answer = cJSON_CreateObject();
    if (answer != NULL && !runtime_catalog_attach(answer, "run", run == NULL ? cJSON_CreateObject() : cJSON_Duplicate(run, 1))) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

/**
 * @implNote Read without the registry lock: it writes nothing, and a references file is replaced at
 * once, never half written. Only an absent file means nothing was recorded; one that exists and cannot
 * be read is refused, since answering it as empty would let a caller start what no start recorded.
 */
cJSON *runtime_updates_recorded_for(const runtime_updates_request *request, cJSON **refusal) {
    char *problem = NULL;
    char *path = runtime_host_references_path(request->state, request->host_id, &problem);
    free(problem);
    if (path == NULL) {
        return NULL;
    }
    cJSON *answer = NULL;
    if (!spisor_file_exists(path)) {
        answer = run_answer(NULL);
    } else {
        cJSON *recorded = runtime_read_node_json(path);
        const cJSON *run = NULL;
        const cJSON *next = NULL;
        const cJSON *rollback = NULL;
        if (runtime_host_references_of(recorded, &run, &next, &rollback)) {
            answer = run_answer(run);
        } else {
            *refusal = unreadable(request, path);
        }
        cJSON_Delete(recorded);
    }
    free(path);
    return answer;
}
