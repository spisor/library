#include "definition_support.h"

#include "host_references.h"
#include "platform_key.h"

spisor_status runtime_check_host_id(spisor_refusal *refusal, const char *host_id, const char *header) {
    if (!runtime_is_text(host_id)) {
        return runtime_refuse_member(refusal, "host_id", header);
    }
    if (!runtime_is_plain_host_id(host_id)) {
        return spisor_refuse_as(refusal, SPISOR_MALFORMED, "the host id is not a plain file name",
                                 RUNTIME_PLAIN_HOST_ID_FIX);
    }
    return SPISOR_OK;
}

spisor_status runtime_check_platform(spisor_refusal *refusal, const char *platform, const char *header) {
    return platform != NULL && !runtime_is_platform_key(platform) ? runtime_refuse_member(refusal, "platform", header)
                                                                  : SPISOR_OK;
}
