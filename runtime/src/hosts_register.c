#include "hosts.h"

#include <stdlib.h>

#include "commands.h"

#include "json_service.h"
#include "registry.h"
#include "spisor/json_kind.h"
#include "user_home.h"

#define IDENTITY_FIX                                                                       \
    "pass the identity {\"id\", \"version\", \"home\": <absolute path>, \"application\"?, " \
    "\"requiresCommand\", \"commands\": [{\"name\", \"target\", \"arguments\"}], "          \
    "\"start\"?: {\"runtime\", \"entry\"}} and the environment {<name>: <value>}"

static int is_command(const cJSON *command) {
    const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(command, "arguments");
    return cJSON_IsObject(command) && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(command, "name")) &&
           cJSON_IsString(cJSON_GetObjectItemCaseSensitive(command, "target")) &&
           (arguments == NULL || cJSON_IsArray(arguments));
}

static int is_identity(const cJSON *identity) {
    const cJSON *commands = cJSON_GetObjectItemCaseSensitive(identity, "commands");
    if (!cJSON_IsObject(identity) || !cJSON_IsArray(commands)) {
        return 0;
    }
    const cJSON *command;
    cJSON_ArrayForEach(command, commands) {
        if (!is_command(command)) {
            return 0;
        }
    }
    return 1;
}

static int copy_value(cJSON *target, const char *target_name, const cJSON *value) {
    cJSON *copy = value == NULL ? NULL : cJSON_Duplicate(value, 1);
    if (value != NULL && (copy == NULL || !cJSON_AddItemToObject(target, target_name, copy))) {
        cJSON_Delete(copy);
        return 0;
    }
    return 1;
}

static int copy_member_as(cJSON *target, const char *target_name, const cJSON *source, const char *name) {
    return copy_value(target, target_name, cJSON_GetObjectItemCaseSensitive(source, name));
}

static int copy_member(cJSON *target, const cJSON *source, const char *name) {
    return copy_member_as(target, name, source, name);
}

/** The registry.register argument for the host identity names, with the given provenance. */
static cJSON *registration_of(const runtime_hosts_registration_request *request, const cJSON *identity) {
    cJSON *registration = cJSON_CreateObject();
    int complete = registration != NULL && copy_member(registration, identity, "id") &&
                   cJSON_AddStringToObject(registration, "kind", "host") != NULL &&
                   copy_member(registration, identity, "version") && copy_member(registration, identity, "home") &&
                   copy_member(registration, identity, "application") &&
                   copy_value(registration, "provenance", request->provenance) &&
                   copy_member(registration, identity, "requiresCommand") && copy_member(registration, identity, "start");
    if (!complete) {
        cJSON_Delete(registration);
        return NULL;
    }
    return registration;
}

/** Registers the host, keeping the provenance of its first start: the record as stored, as the registry answers it. */
static cJSON *register_first(const runtime_hosts_registration_request *request, const cJSON *identity,
                             cJSON **refusal) {
    cJSON *registration = registration_of(request, identity);
    cJSON *registered = registration == NULL
                            ? NULL
                            : runtime_registry_register_first_provenance(request->state, registration, refusal);
    if (registration == NULL) {
        *refusal = NULL;
    }
    cJSON_Delete(registration);
    return registered;
}

/** The commands request for one command of the host identity names, borrowing from the registration and command. */
static runtime_command_request command_request(const runtime_hosts_registration_request *registration,
                                               const cJSON *identity, const cJSON *command, const char *home) {
    runtime_command_request request = {registration->state,
                                       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(identity, "id")),
                                       registration->environment,
                                       home,
                                       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(command, "name")),
                                       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(command, "target")),
                                       cJSON_GetObjectItemCaseSensitive(command, "arguments"),
                                       NULL,
                                       NULL};
    return request;
}

static int add_refused(cJSON *refused_commands, const cJSON *command, const cJSON *refusal) {
    cJSON *refused = runtime_is_refusal(refusal) ? cJSON_CreateObject() : NULL;
    int added = refused != NULL && copy_member(refused, command, "name") &&
                cJSON_AddStringToObject(refused, "detail",
                                        cJSON_GetObjectItemCaseSensitive(refusal, "message")->valuestring) != NULL &&
                copy_member(refused, refusal, "fix") && cJSON_AddItemToArray(refused_commands, refused);
    if (!added) {
        cJSON_Delete(refused);
    }
    return added;
}

static int add_name(cJSON *commands, const char *name) {
    if (name[0] == '\0') {
        return 1;
    }
    cJSON *copy = cJSON_CreateString(name);
    if (copy == NULL || !cJSON_AddItemToArray(commands, copy)) {
        cJSON_Delete(copy);
        return 0;
    }
    return 1;
}

/**
 * Requests one command into answer's commands or refusedCommands; 0 with *refusal set when the host
 * requires a command and this one was refused, 0 with none when memory runs out.
 *
 * @implNote Only the runtime's refusal is recorded, and only for a host that can run without a command;
 * a host that requires one fails to start instead.
 */
static int request_command(const runtime_hosts_registration_request *registration, const cJSON *identity,
                           const cJSON *command, const char *home, cJSON *answer, cJSON **refusal) {
    spisor_refusal refused_by = SPISOR_INIT(spisor_refusal);
    char name[RUNTIME_COMMAND_NAME_SIZE];
    runtime_command_request request = command_request(registration, identity, command, home);
    spisor_status status = runtime_commands_serve(RUNTIME_COMMAND_REQUEST, &request, name, &refused_by);
    cJSON *refused = status == SPISOR_OK ? NULL : runtime_refusal_of(&refused_by);
    spisor_refusal_release(&refused_by);
    int complete = status == SPISOR_OK && add_name(cJSON_GetObjectItemCaseSensitive(answer, "commands"), name);
    if (refused != NULL && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(identity, "requiresCommand"))) {
        *refusal = refused;
        return 0;
    }
    if (refused != NULL) {
        complete = add_refused(cJSON_GetObjectItemCaseSensitive(answer, "refusedCommands"), command, refused);
        cJSON_Delete(refused);
    }
    return complete;
}

static cJSON *registered_answer(cJSON *registered) {
    cJSON *provenance = cJSON_DetachItemFromObjectCaseSensitive(registered, "provenance");
    cJSON_Delete(registered);
    cJSON *answer = provenance == NULL ? NULL : cJSON_CreateObject();
    if (answer == NULL || !cJSON_AddItemToObject(answer, "provenance", provenance)) {
        cJSON_Delete(answer);
        cJSON_Delete(provenance);
        return NULL;
    }
    if (cJSON_AddArrayToObject(answer, "commands") == NULL || cJSON_AddArrayToObject(answer, "refusedCommands") == NULL) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static cJSON *request_commands(const runtime_hosts_registration_request *registration, const cJSON *identity,
                               const char *home, cJSON *answer, cJSON **refusal) {
    const cJSON *command;
    cJSON_ArrayForEach(command, cJSON_GetObjectItemCaseSensitive(identity, "commands")) {
        if (!request_command(registration, identity, command, home, answer, refusal)) {
            cJSON_Delete(answer);
            return NULL;
        }
    }
    return answer;
}

cJSON *runtime_hosts_register(const runtime_hosts_registration_request *request, cJSON **refusal) {
    const cJSON *identity = request->identity;
    const cJSON *environment = request->environment;
    *refusal = NULL;
    if (!is_identity(identity) || !cJSON_IsObject(environment)) {
        *refusal = runtime_refusal("the call carries no usable identity or environment", IDENTITY_FIX);
        return NULL;
    }
    const char *home = runtime_user_home_of(environment);
    if (home == NULL && cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(identity, "commands")) > 0) {
        *refusal = runtime_refusal(RUNTIME_NO_USER_HOME_MESSAGE, RUNTIME_NO_USER_HOME_FIX);
        return NULL;
    }
    cJSON *registered = register_first(request, identity, refusal);
    cJSON *answer = registered == NULL ? NULL : registered_answer(registered);
    return answer == NULL ? NULL : request_commands(request, identity, home, answer, refusal);
}
