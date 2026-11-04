#include "hosts.h"

#include <stdlib.h>

#include "commands.h"
#include "hosts_request.h"
#include "json_service.h"
#include "registry.h"
#include "spisor/json_kind.h"
#include "user_home.h"

#define IDENTITY_FIX                                                                                                \
    "send {\"identity\": {\"id\", \"version\", \"home\"?, \"application\"?, \"requiresCommand\", \"commands\": "    \
    "[{\"name\", \"target\", \"arguments\"}], \"start\"?: {\"runtime\", \"entry\"}}, \"provenance\": {...}, "        \
    "\"environment\": {<name>: <value>}}"

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

static int copy_member_as(cJSON *target, const char *target_name, const cJSON *source, const char *name) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(source, name);
    cJSON *copy = value == NULL ? NULL : cJSON_Duplicate(value, 1);
    if (value != NULL && (copy == NULL || !cJSON_AddItemToObject(target, target_name, copy))) {
        cJSON_Delete(copy);
        return 0;
    }
    return 1;
}

static int copy_member(cJSON *target, const cJSON *source, const char *name) {
    return copy_member_as(target, name, source, name);
}

/** The registry.register argument for the host identity names, with the given provenance. */
static cJSON *registration_request(const cJSON *argument, const cJSON *identity) {
    cJSON *request = runtime_hosts_places_request(argument);
    cJSON *registration = request == NULL ? NULL : cJSON_AddObjectToObject(request, "registration");
    int complete = registration != NULL && copy_member(registration, identity, "id") &&
                   cJSON_AddStringToObject(registration, "kind", "host") != NULL &&
                   copy_member(registration, identity, "version") && copy_member(registration, identity, "home") &&
                   copy_member(registration, identity, "application") && copy_member(registration, argument, "provenance") &&
                   copy_member(registration, identity, "requiresCommand") && copy_member(registration, identity, "start");
    if (!complete) {
        cJSON_Delete(request);
        return NULL;
    }
    return request;
}

static cJSON *command_request(const cJSON *argument, const cJSON *identity, const cJSON *command, const char *home) {
    cJSON *request = runtime_hosts_places_request(argument);
    const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(command, "arguments");
    int complete = request != NULL && copy_member(request, argument, "environment") &&
                   copy_member_as(request, "host", identity, "id") &&
                   (home == NULL || cJSON_AddStringToObject(request, "home", home) != NULL) &&
                   copy_member(request, command, "name") && copy_member(request, command, "target") &&
                   (arguments == NULL || copy_member(request, command, "arguments"));
    if (!complete) {
        cJSON_Delete(request);
        return NULL;
    }
    return request;
}

static int add_refused(cJSON *refused_commands, const cJSON *command, const cJSON *refusal) {
    cJSON *refused = cJSON_CreateObject();
    int added = refused != NULL && copy_member(refused, command, "name") &&
                cJSON_AddStringToObject(refused, "detail",
                                        cJSON_GetObjectItemCaseSensitive(refusal, "message")->valuestring) != NULL &&
                copy_member(refused, refusal, "fix") && cJSON_AddItemToArray(refused_commands, refused);
    if (!added) {
        cJSON_Delete(refused);
    }
    return added;
}

static int add_name(cJSON *commands, const cJSON *answer) {
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(answer, "name");
    if (!cJSON_IsString(name)) {
        return 1;
    }
    cJSON *copy = cJSON_Duplicate(name, 1);
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
static int request_command(const cJSON *argument, const cJSON *identity, const cJSON *command, const char *home,
                           cJSON *answer, cJSON **refusal) {
    cJSON *refused = NULL;
    cJSON *requested = runtime_call_or_refuse(runtime_commands_request,
                                              command_request(argument, identity, command, home), &refused);
    int complete = requested != NULL && add_name(cJSON_GetObjectItemCaseSensitive(answer, "commands"), requested);
    cJSON_Delete(requested);
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
    cJSON *provenance = cJSON_DetachItemFromObjectCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(registered, "registration"), "provenance");
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

static cJSON *request_commands(const cJSON *argument, const cJSON *identity, const char *home, cJSON *answer,
                               cJSON **refusal) {
    const cJSON *command;
    cJSON_ArrayForEach(command, cJSON_GetObjectItemCaseSensitive(identity, "commands")) {
        if (!request_command(argument, identity, command, home, answer, refusal)) {
            cJSON_Delete(answer);
            return NULL;
        }
    }
    return answer;
}

static cJSON *register_for(const cJSON *argument, cJSON **refusal) {
    const cJSON *identity = cJSON_GetObjectItemCaseSensitive(argument, "identity");
    const cJSON *environment = cJSON_GetObjectItemCaseSensitive(argument, "environment");
    if (!is_identity(identity) || !cJSON_IsObject(environment)) {
        *refusal = runtime_refusal("the argument carries no usable identity or environment", IDENTITY_FIX);
        return NULL;
    }
    const char *home = runtime_user_home_of(environment);
    if (home == NULL && cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(identity, "commands")) > 0) {
        *refusal = runtime_refusal(RUNTIME_NO_USER_HOME_MESSAGE, RUNTIME_NO_USER_HOME_FIX);
        return NULL;
    }
    cJSON *registered = runtime_call_or_refuse(runtime_registry_register_first_provenance,
                                               registration_request(argument, identity), refusal);
    cJSON *answer = registered == NULL ? NULL : registered_answer(registered);
    return answer == NULL ? NULL : request_commands(argument, identity, home, answer, refusal);
}

RUNTIME_EXPORT int32_t runtime_hosts_register(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, register_for);
}
