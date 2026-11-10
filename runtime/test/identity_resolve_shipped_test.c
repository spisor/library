#include <string.h>

#include "catalog_call.h"
#include "check.h"
#include "identity_resolve.h"

static void refuses_an_api_base_in_the_shipped_library(void) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "id", "github:7");
    cJSON_AddStringToObject(argument, "apiBase", "https://attacker.example");
    catalog_answer answer = call_catalog(runtime_identity_resolve, argument);
    CHECK(expect_refusal(answer, "apiBase", "shipped apiBase"));
    cJSON_Delete(answer.tree);
}

static void still_resolves_local_in_the_shipped_library(void) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "id", "local:plugin-stub");
    catalog_answer answer = call_catalog(runtime_identity_resolve, argument);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(answer.tree);
}

int main(void) {
    refuses_an_api_base_in_the_shipped_library();
    still_resolves_local_in_the_shipped_library();
    return check_summary("identity_resolve_shipped_test");
}
