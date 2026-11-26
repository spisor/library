#include <stdio.h>
#include <string.h>

#include "allocation.h"
#include "plugin_scope.h"
#include "secrets.h"
#include "spisor/definition.h"
#include "standard_refusal.h"
#include "naula/json.h"

#define SECRET_SHAPE SPISOR_SHAPE_THROUGH(weivel_standard_secret, storage)

/**
 * What runtime's secrets service answered, as the scope answers it: the scope's own checks came first, so a MALFORMED
 * from runtime refuses what the caller passed and runtime will not keep, such as an empty name.
 */
static spisor_status as_scope_answer(spisor_status status) {
    return status == SPISOR_MALFORMED ? SPISOR_REFUSED : status;
}

static spisor_status refuse_missing_name(spisor_refusal *refusal, const char *operation) {
    char detail[64];
    snprintf(detail, sizeof detail, "secrets' %s was called without a name", operation);
    return spisor_refuse_as(refusal, SPISOR_MALFORMED, detail, "pass the secret's name, such as \"token\"");
}

static weivel_status remove_secret(void *context, const char *name, weivel_standard_service_refusal *refusal) {
    const runtime_plugin_scope *scope = context;
    runtime_secret_places places = runtime_plugin_scope_secret_places(scope);
    spisor_refusal filled = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    if (!runtime_is_usable_service_refusal(refusal)) {
        return WEIVEL_STATUS_MALFORMED;
    }
    if (name == NULL) {
        return runtime_answer_service(refuse_missing_name(&filled, "remove"), &filled, refusal);
    }
    status = as_scope_answer(runtime_delete_secret(&places, scope->manifest.id, name, &filled));
    return runtime_answer_service(status, &filled, refusal);
}

static weivel_status get_secret(void *context, const char *name, weivel_standard_secret *secret,
                                weivel_standard_service_refusal *refusal) {
    const runtime_plugin_scope *scope = context;
    runtime_secret_places places = runtime_plugin_scope_secret_places(scope);
    spisor_refusal filled = SPISOR_INIT(spisor_refusal);
    char *value = NULL;
    spisor_status status;
    if (!runtime_is_usable_service_refusal(refusal)) {
        return WEIVEL_STATUS_MALFORMED;
    }
    if (!SPISOR_IS_EMPTY_COUNTED_ANSWER(secret, SECRET_SHAPE, secret->length) || secret->bytes != NULL) {
        return runtime_answer_service(spisor_refuse_shape(&filled, "secrets' get"), &filled, refusal);
    }
    if (name == NULL) {
        return runtime_answer_service(refuse_missing_name(&filled, "get"), &filled, refusal);
    }
    status = as_scope_answer(runtime_get_secret(&places, scope->manifest.id, name, &value, &filled));
    if (status == SPISOR_OK) {
        secret->bytes = (const uint8_t *)value;
        secret->length = strlen(value);
        secret->storage = value;
        runtime_zero_beyond(secret, sizeof *secret);
    }
    return runtime_answer_service(status, &filled, refusal);
}

static spisor_status refuse_bytes(spisor_refusal *refusal, spisor_status status, const char *problem) {
    char detail[160];
    snprintf(detail, sizeof detail, "secrets' set was given %s", problem);
    return spisor_refuse_as(refusal, status, detail,
                            "store the secret as UTF-8 text without a NUL byte, which every place runtime keeps "
                            "secrets in holds");
}

/** The refusal of bytes runtime cannot keep, or OK; runtime keeps a secret as text in its file and the OS store. */
static spisor_status check_bytes(const uint8_t *bytes, size_t length, spisor_refusal *refusal) {
    if (bytes == NULL && length != 0) {
        return refuse_bytes(refusal, SPISOR_MALFORMED, "a length without its bytes");
    }
    if (length != 0 && memchr(bytes, '\0', length) != NULL) {
        return refuse_bytes(refusal, SPISOR_REFUSED, "bytes holding a NUL byte");
    }
    if (length != 0 && !naula_json_is_valid_utf8(bytes, length)) {
        return refuse_bytes(refusal, SPISOR_REFUSED, "bytes that are not well-formed UTF-8");
    }
    return SPISOR_OK;
}

static spisor_status store_text(const runtime_plugin_scope *scope, const char *name, const uint8_t *bytes,
                                size_t length, spisor_refusal *refusal) {
    runtime_secret_places places = runtime_plugin_scope_secret_places(scope);
    runtime_secret_location location;
    spisor_status status;
    char *text = spisor_memory_allocate(length + 1);
    if (text == NULL) {
        return SPISOR_OUT_OF_MEMORY;
    }
    if (length != 0) {
        memcpy(text, bytes, length);
    }
    text[length] = '\0';
    status = as_scope_answer(runtime_set_secret(&places, scope->manifest.id, name, text, &location, refusal));
    spisor_overwrite(text, length);
    spisor_memory_release(text);
    return status;
}

static weivel_status set_secret(void *context, const char *name, const uint8_t *bytes, size_t length,
                                weivel_standard_service_refusal *refusal) {
    spisor_refusal filled = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    if (!runtime_is_usable_service_refusal(refusal)) {
        return WEIVEL_STATUS_MALFORMED;
    }
    if (name == NULL) {
        return runtime_answer_service(refuse_missing_name(&filled, "set"), &filled, refusal);
    }
    status = check_bytes(bytes, length, &filled);
    if (status == SPISOR_OK) {
        status = store_text(context, name, bytes, length, &filled);
    }
    return runtime_answer_service(status, &filled, refusal);
}

static void release_secret(void *context, weivel_standard_secret *secret) {
    (void)context;
    if (secret == NULL || !spisor_has_shape(secret->size, SECRET_SHAPE)) {
        return;
    }
    if (secret->storage != NULL) {
        spisor_overwrite(secret->storage, secret->length);
    }
    spisor_memory_release(secret->storage);
    SPISOR_CLEAR_ANSWER(secret, SECRET_SHAPE);
}

void runtime_scope_secrets_fill(runtime_plugin_scope *scope) {
    weivel_standard_secrets *secrets = &scope->secrets;
    secrets->size = (uint32_t)sizeof *secrets;
    secrets->context = scope;
    secrets->remove = remove_secret;
    secrets->get = get_secret;
    secrets->set = set_secret;
    secrets->release_secret = release_secret;
    secrets->release_refusal = runtime_release_service_refusal;
}
