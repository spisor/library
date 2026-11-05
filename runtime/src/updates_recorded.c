#include <stddef.h>

#include "catalog_answer.h"
#include "host_references.h"
#include "updates_request.h"

/** @implNote Read without the registry lock: it writes nothing, and a references file is replaced at once, never half written. */
cJSON *runtime_updates_recorded_for(const runtime_updates_request *request, cJSON **refusal) {
    (void)refusal;
    runtime_host_references recorded = runtime_read_host_references(request->state, request->host_id);
    cJSON *answer = recorded.run == NULL ? NULL : cJSON_CreateObject();
    if (answer != NULL && !runtime_catalog_attach(answer, "run", cJSON_Duplicate(recorded.run, 1))) {
        cJSON_Delete(answer);
        answer = NULL;
    }
    runtime_free_host_references(&recorded);
    return answer;
}
