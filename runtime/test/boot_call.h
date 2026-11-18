#ifndef RUNTIME_TEST_BOOT_CALL_H
#define RUNTIME_TEST_BOOT_CALL_H

#include <stdlib.h>
#include <string.h>

#include "spisor/allocation.h"
#include "cJSON.h"
#include "catalog_call.h"
#include "check.h"
#include "runtime/boot.h"
#include "spisor/status.h"

/**
 * @file
 * Calls the boot definition with a request a test writes as JSON, in the shape boot's operations take inside runtime,
 * and answers what the definition answered as that operation's JSON, so a test reads an answer the way runtime's own
 * callers do. A refusal answers {message, fix} with the definition's status. Test code only: the JSON is the test's
 * data, never the boundary.
 */

typedef enum boot_operation {
    BOOT_READ,
    BOOT_WRITE,
    BOOT_RESOLVE,
    BOOT_PLACE
} boot_operation;

static inline const char *boot_text(const cJSON *object, const char *name) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
}

static inline void boot_add_text(cJSON *object, const char *name, const char *text) {
    if (text != NULL) {
        CHECK(cJSON_AddStringToObject(object, name, text) != NULL);
    }
}

static inline cJSON *boot_hint_json(const runtime_boot_hint_answer *answer) {
    cJSON *tree = cJSON_CreateObject();
    if (answer->hinted) {
        cJSON *hint = cJSON_AddObjectToObject(tree, "hint");
        cJSON *libraries = cJSON_AddObjectToObject(hint, "libraries");
        for (size_t index = 0; index < answer->library_count; index++) {
            cJSON *library = cJSON_AddObjectToObject(libraries, answer->libraries[index].id);
            boot_add_text(library, "version", answer->libraries[index].version);
            boot_add_text(library, "directory", answer->libraries[index].directory);
            boot_add_text(library, "binary", answer->libraries[index].binary);
        }
        boot_add_text(hint, "host", answer->host);
    }
    return tree;
}

static inline spisor_status boot_read_call(const runtime_boot_definition *boot, const cJSON *request, cJSON **tree,
                                           spisor_refusal *refusal) {
    runtime_boot_host_argument argument = SPISOR_INIT(runtime_boot_host_argument);
    runtime_boot_hint_answer answer = SPISOR_INIT(runtime_boot_hint_answer);
    spisor_status status;
    argument.cache = boot_text(request, "cache");
    argument.host_id = boot_text(request, "hostId");
    status = boot->read(boot->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        *tree = boot_hint_json(&answer);
    }
    boot->release_hint_answer(boot->context, &answer);
    return status;
}

static inline spisor_status boot_write_call(const runtime_boot_definition *boot, const cJSON *request, cJSON **tree,
                                            spisor_refusal *refusal) {
    const cJSON *versions = cJSON_GetObjectItemCaseSensitive(request, "libraries");
    runtime_boot_write_argument argument = SPISOR_INIT(runtime_boot_write_argument);
    runtime_boot_hint_answer answer = SPISOR_INIT(runtime_boot_hint_answer);
    runtime_item_version *libraries = spisor_memory_allocate_zeroed((size_t)cJSON_GetArraySize(versions) + 1,
            sizeof *libraries);
    const cJSON *member;
    spisor_status status;
    CHECK(libraries != NULL);
    cJSON_ArrayForEach(member, versions) {
        libraries[argument.library_count].id = member->string;
        libraries[argument.library_count++].version = cJSON_GetStringValue(member);
    }
    argument.cache = boot_text(request, "cache");
    argument.host_id = boot_text(request, "hostId");
    argument.libraries = libraries;
    argument.host = boot_text(request, "host");
    argument.state = boot_text(request, "state");
    argument.platform = boot_text(request, "platform");
    status = boot->write(boot->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        *tree = boot_hint_json(&answer);
    }
    boot->release_hint_answer(boot->context, &answer);
    spisor_memory_release(libraries);
    return status;
}

static inline spisor_status boot_resolve_call(const runtime_boot_definition *boot, const cJSON *request, cJSON **tree,
                                              spisor_refusal *refusal) {
    const cJSON *given = cJSON_GetObjectItemCaseSensitive(request, "arguments");
    runtime_boot_resolve_argument argument = SPISOR_INIT(runtime_boot_resolve_argument);
    runtime_boot_resolve_answer answer = SPISOR_INIT(runtime_boot_resolve_answer);
    const char **arguments = spisor_memory_allocate_zeroed((size_t)cJSON_GetArraySize(given) + 1, sizeof *arguments);
    const cJSON *entry;
    spisor_status status;
    CHECK(arguments != NULL);
    cJSON_ArrayForEach(entry, given) {
        arguments[argument.argument_count++] = cJSON_GetStringValue(entry);
    }
    argument.cache = boot_text(request, "cache");
    argument.host_id = boot_text(request, "hostId");
    argument.arguments = arguments;
    argument.platform = boot_text(request, "platform");
    status = boot->resolve(boot->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        cJSON *passed = cJSON_CreateArray();
        *tree = cJSON_CreateObject();
        boot_add_text(*tree, "executable", answer.executable);
        boot_add_text(*tree, "version", answer.version);
        boot_add_text(*tree, "directory", answer.directory);
        for (size_t index = 0; index < answer.argument_count; index++) {
            CHECK(cJSON_AddItemToArray(passed, cJSON_CreateString(answer.arguments[index])));
        }
        cJSON_AddItemToObject(*tree, "arguments", passed);
    }
    boot->release_resolve_answer(boot->context, &answer);
    spisor_memory_release((void *)arguments);
    return status;
}

static inline spisor_status boot_place_call(const runtime_boot_definition *boot, const cJSON *request, cJSON **tree,
                                            spisor_refusal *refusal) {
    runtime_boot_place_argument argument = SPISOR_INIT(runtime_boot_place_argument);
    runtime_boot_place_answer answer = SPISOR_INIT(runtime_boot_place_answer);
    spisor_status status;
    argument.cache = boot_text(request, "cache");
    status = boot->place(boot->context, &argument, &answer, refusal);
    if (status == SPISOR_OK) {
        *tree = cJSON_CreateObject();
        cJSON_AddBoolToObject(*tree, "placed", answer.placed != 0);
        boot_add_text(*tree, "path", answer.path);
    }
    boot->release_place_answer(boot->context, &answer);
    return status;
}

/** Calls `operation` with `request`, which it frees, answering the definition's status and the answer as JSON. */
static inline catalog_answer boot_call(boot_operation operation, cJSON *request) {
    const runtime_boot_definition *boot = runtime_boot_definition_of();
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    catalog_answer result = {0, NULL};
    spisor_status status;
    switch (operation) {
    case BOOT_READ:
        status = boot_read_call(boot, request, &result.tree, &refusal);
        break;
    case BOOT_WRITE:
        status = boot_write_call(boot, request, &result.tree, &refusal);
        break;
    case BOOT_RESOLVE:
        status = boot_resolve_call(boot, request, &result.tree, &refusal);
        break;
    default:
        status = boot_place_call(boot, request, &result.tree, &refusal);
        break;
    }
    if (status != SPISOR_OK) {
        result.tree = cJSON_CreateObject();
        boot_add_text(result.tree, "message", refusal.detail);
        boot_add_text(result.tree, "fix", refusal.fix);
    }
    boot->release_refusal(boot->context, &refusal);
    cJSON_Delete(request);
    result.status = (int32_t)status;
    return result;
}

#endif
