#include <stdio.h>
#include <string.h>

#include "allocation.h"
#include "check.h"
#include "definition_support.h"
#include "spisor/json_allocation.h"
#include "runtime/commands.h"
#include "runtime/detect.h"
#include "runtime/install.h"
#include "runtime/lists.h"
#include "runtime/places.h"
#include "runtime/processes.h"

#if defined(_WIN32)
#define ABSOLUTE_PLACE "C:\\spisor-environment-check"
#else
#define ABSOLUTE_PLACE "/spisor-environment-check"
#endif

/** One environment every service refuses, as an array and its count. */
typedef struct refused_environment {
    const char *label;
    const runtime_variable *variables;
    size_t count;
} refused_environment;

/** Calls one operation with `environment`, answering its status and filling `refusal`. */
typedef spisor_status (*environment_call)(const refused_environment *environment, spisor_refusal *refusal);

static spisor_status call_places(const refused_environment *environment, spisor_refusal *refusal) {
    const runtime_places_definition *places = runtime_places_definition_of();
    runtime_places_resolve_argument argument = SPISOR_INIT(runtime_places_resolve_argument);
    runtime_places_answer answer = SPISOR_INIT(runtime_places_answer);
    argument.environment = environment->variables;
    argument.environment_count = environment->count;
    return places->resolve(places->context, &argument, &answer, refusal);
}

static spisor_status call_detect(const refused_environment *environment, spisor_refusal *refusal) {
    const runtime_detect_definition *detect = runtime_detect_definition_of();
    runtime_detect_on_path_argument argument = SPISOR_INIT(runtime_detect_on_path_argument);
    runtime_detect_on_path_answer answer = SPISOR_INIT(runtime_detect_on_path_answer);
    argument.binary = "claude";
    argument.environment = environment->variables;
    argument.environment_count = environment->count;
    return detect->on_path(detect->context, &argument, &answer, refusal);
}

static spisor_status call_lists(const refused_environment *environment, spisor_refusal *refusal) {
    const runtime_lists_definition *lists = runtime_lists_definition_of();
    runtime_lists_applications_argument argument = SPISOR_INIT(runtime_lists_applications_argument);
    runtime_list answer = SPISOR_INIT(runtime_list);
    argument.state = ABSOLUTE_PLACE;
    argument.environment = environment->variables;
    argument.environment_count = environment->count;
    return lists->applications(lists->context, &argument, &answer, refusal);
}

static void command_argument_of(const refused_environment *environment, runtime_command_argument *argument) {
    argument->state = ABSOLUTE_PLACE;
    argument->host = "spisor-host";
    argument->environment = environment->variables;
    argument->environment_count = environment->count;
}

static spisor_status call_command_request(const refused_environment *environment, spisor_refusal *refusal) {
    const runtime_commands_definition *commands = runtime_commands_definition_of();
    runtime_command_argument argument = SPISOR_INIT(runtime_command_argument);
    runtime_command_answer answer = SPISOR_INIT(runtime_command_answer);
    command_argument_of(environment, &argument);
    return commands->request(commands->context, &argument, &answer, refusal);
}

static spisor_status call_command_add(const refused_environment *environment, spisor_refusal *refusal) {
    const runtime_commands_definition *commands = runtime_commands_definition_of();
    runtime_command_argument argument = SPISOR_INIT(runtime_command_argument);
    runtime_command_answer answer = SPISOR_INIT(runtime_command_answer);
    command_argument_of(environment, &argument);
    return commands->add(commands->context, &argument, &answer, refusal);
}

static spisor_status call_command_rename(const refused_environment *environment, spisor_refusal *refusal) {
    const runtime_commands_definition *commands = runtime_commands_definition_of();
    runtime_commands_rename_argument argument = SPISOR_INIT(runtime_commands_rename_argument);
    runtime_command_answer answer = SPISOR_INIT(runtime_command_answer);
    argument.state = ABSOLUTE_PLACE;
    argument.host = "spisor-host";
    argument.environment = environment->variables;
    argument.environment_count = environment->count;
    return commands->rename(commands->context, &argument, &answer, refusal);
}

static spisor_status call_install_runtime(const refused_environment *environment, spisor_refusal *refusal) {
    const runtime_install_definition *install = runtime_install_definition_of();
    runtime_install_runtime_argument argument = SPISOR_INIT(runtime_install_runtime_argument);
    runtime_install_runtime_answer answer = SPISOR_INIT(runtime_install_runtime_answer);
    argument.cache = ABSOLUTE_PLACE;
    argument.environment = environment->variables;
    argument.environment_count = environment->count;
    return install->runtime(install->context, &argument, &answer, refusal);
}

static spisor_status call_install_register_loader(const refused_environment *environment, spisor_refusal *refusal) {
    const runtime_install_definition *install = runtime_install_definition_of();
    runtime_install_register_loader_argument argument = SPISOR_INIT(runtime_install_register_loader_argument);
    runtime_install_register_loader_answer answer = SPISOR_INIT(runtime_install_register_loader_answer);
    argument.environment = environment->variables;
    argument.environment_count = environment->count;
    return install->register_loader(install->context, &argument, &answer, refusal);
}

static spisor_status call_install_ecosystem(const refused_environment *environment, spisor_refusal *refusal) {
    const runtime_install_definition *install = runtime_install_definition_of();
    runtime_install_ecosystem_argument argument = SPISOR_INIT(runtime_install_ecosystem_argument);
    runtime_install_ecosystem_answer answer = SPISOR_INIT(runtime_install_ecosystem_answer);
    argument.environment = environment->variables;
    argument.environment_count = environment->count;
    return install->ecosystem(install->context, &argument, &answer, refusal);
}

static spisor_status call_processes_hold(const refused_environment *environment, spisor_refusal *refusal) {
    const runtime_processes_definition *processes = runtime_processes_definition_of();
    runtime_processes_hold_argument argument = SPISOR_INIT(runtime_processes_hold_argument);
    runtime_processes_hold_answer answer = SPISOR_INIT(runtime_processes_hold_answer);
    argument.state = ABSOLUTE_PLACE;
    argument.host_id = "background-host";
    argument.holder = 1;
    argument.executable = ABSOLUTE_PLACE;
    argument.environment = environment->variables;
    argument.environment_count = environment->count;
    return processes->hold(processes->context, &argument, &answer, refusal);
}

static spisor_status call_processes_ready(const refused_environment *environment, spisor_refusal *refusal) {
    const runtime_processes_definition *processes = runtime_processes_definition_of();
    runtime_processes_ready_argument argument = SPISOR_INIT(runtime_processes_ready_argument);
    argument.state = ABSOLUTE_PLACE;
    argument.host_id = "session-host";
    argument.pid = 1;
    argument.environment = environment->variables;
    argument.environment_count = environment->count;
    return processes->ready(processes->context, &argument, refusal);
}

/** The allocations filling the environment refusal takes on its own, which a refusing call may take and no more. */
static uint64_t refusal_allocations(void) {
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_memory_fail_at(0);
    CHECK(runtime_refuse_environment(&refusal) == SPISOR_MALFORMED);
    uint64_t allocations = spisor_memory_attempts();
    spisor_release_refusal(NULL, &refusal);
    return allocations;
}

static void it_refuses_before_allocating(const char *operation, environment_call call,
                                         const refused_environment *environment, uint64_t allowed) {
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_memory_fail_at(0);
    spisor_status status = call(environment, &refusal);
    uint64_t allocations = spisor_memory_attempts();
    int refused = status == SPISOR_MALFORMED && refusal.detail != NULL &&
                  strstr(refusal.detail, "environment variable") != NULL;
    if (!refused || allocations != allowed) {
        fprintf(stderr, "%s with %s: answered %d \"%s\" after %llu allocations, %llu allowed\n", operation,
                environment->label, (int)status, refusal.detail == NULL ? "" : refusal.detail,
                (unsigned long long)allocations, (unsigned long long)allowed);
        check_failures++;
    }
    spisor_release_refusal(NULL, &refusal);
}

int main(void) {
    static const runtime_variable holding_equals[1] = {{"A=B", "1"}};
    static const runtime_variable empty_name[1] = {{"", "1"}};
    static const runtime_variable named_twice[2] = {{"A", "1"}, {"A", "2"}};
    static const runtime_variable no_value[1] = {{"A", NULL}};
    static const refused_environment environments[5] = {{"a name holding =", holding_equals, 1},
                                                        {"an empty name", empty_name, 1},
                                                        {"a name given twice", named_twice, 2},
                                                        {"a variable without a value", no_value, 1},
                                                        {"a count without variables", NULL, 1}};
    static const struct {
        const char *operation;
        environment_call call;
    } calls[11] = {{"places' resolve", call_places},
                   {"detect's on_path", call_detect},
                   {"lists' applications", call_lists},
                   {"commands' request", call_command_request},
                   {"commands' add", call_command_add},
                   {"commands' rename", call_command_rename},
                   {"install's runtime", call_install_runtime},
                   {"install's register_loader", call_install_register_loader},
                   {"install's ecosystem", call_install_ecosystem},
                   {"processes' hold", call_processes_hold},
                   {"processes' ready", call_processes_ready}};
    spisor_json_use_library_allocator();
    uint64_t allowed = refusal_allocations();
    for (size_t call = 0; call < 11; call++) {
        for (size_t environment = 0; environment < 5; environment++) {
            it_refuses_before_allocating(calls[call].operation, calls[call].call, &environments[environment], allowed);
        }
    }
    return check_summary("environment_check_test");
}
