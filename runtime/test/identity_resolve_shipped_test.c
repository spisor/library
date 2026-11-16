#include <string.h>

#include "check.h"
#include "definition_fixture.h"
#include "runtime/identity.h"

static spisor_status resolve(const char *id, const char *api_base, runtime_identity_repository *answer,
                             spisor_refusal *refusal) {
    const runtime_identity_definition *identity = runtime_identity_definition_of();
    runtime_identity_resolve_argument argument = SPISOR_INIT(runtime_identity_resolve_argument);
    argument.id = id;
    argument.api_base = api_base;
    return identity->resolve(identity->context, &argument, answer, refusal);
}

static void refuses_an_api_base_in_the_shipped_library(void) {
    const runtime_identity_definition *identity = runtime_identity_definition_of();
    runtime_identity_repository answer = SPISOR_INIT(runtime_identity_repository);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    check_refused(resolve("github:7", "https://attacker.example", &answer, &refusal), SPISOR_REFUSED, &refusal,
                  "apiBase", "shipped apiBase");
    CHECK(answer.storage == NULL);
    identity->release_refusal(identity->context, &refusal);
}

static void still_resolves_local_in_the_shipped_library(void) {
    const runtime_identity_definition *identity = runtime_identity_definition_of();
    runtime_identity_repository answer = SPISOR_INIT(runtime_identity_repository);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    CHECK(resolve("local:plugin-stub", NULL, &answer, &refusal) == SPISOR_OK);
    check_text(answer.full_name, "plugin-stub", "local full name");
    identity->release_repository(identity->context, &answer);
}

int main(void) {
    refuses_an_api_base_in_the_shipped_library();
    still_resolves_local_in_the_shipped_library();
    return check_summary("identity_resolve_shipped_test");
}
