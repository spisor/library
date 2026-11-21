#include "described_checks.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <weivel/standard.h>
#include <weivel/value.h>

#include "erased_calls.h"

#define ARGUMENT_LIMIT 6
#define LABEL_BYTES 256
#define NONEMPTY_COUNT 3

typedef void (*erased_function)(void);
typedef weivel_status (*operation_of_1)(void *, void *);
typedef weivel_status (*operation_of_2)(void *, void *, void *);
typedef weivel_status (*operation_of_3)(void *, void *, void *, void *);
typedef weivel_status (*operation_of_4)(void *, void *, void *, void *, void *);
typedef weivel_status (*operation_of_5)(void *, void *, void *, void *, void *, void *);
typedef weivel_status (*operation_of_6)(void *, void *, void *, void *, void *, void *, void *);
typedef void (*release_function)(void *, void *);

typedef enum parameter_rule {
    RULE_UNREADABLE,
    RULE_CONTEXT,
    RULE_STRUCT_INPUT,
    RULE_STRUCT_ANSWER,
    RULE_TREE_ANSWER,
    RULE_NULL_INPUT,
    RULE_SLOT_ANSWER,
    RULE_REFUSAL
} parameter_rule;

typedef struct definition_view {
    const weivel_standard_description *description;
    const void *definition;
    void *context;
} definition_view;

/** The bytes a size-prefixed parameter has, and how many of them its first shape reaches. */
typedef struct parameter_shape {
    size_t size;
    size_t first_shape;
} parameter_shape;

/** One change to an operation's empty parameters: a `size` one short of its first shape, or one count nonzero. */
typedef struct parameter_change {
    size_t parameter;
    size_t count_offset;
    int shortens;
    const char *refused;
    const char *label;
} parameter_change;

/** One buffer per parameter after the context, in the order the operation takes them. */
typedef struct prepared_call {
    void *arguments[ARGUMENT_LIMIT];
    unsigned char *buffers[ARGUMENT_LIMIT];
    unsigned char *before[ARGUMENT_LIMIT];
    size_t sizes[ARGUMENT_LIMIT];
    int answers[ARGUMENT_LIMIT];
    size_t argument_count;
    size_t refusal_argument;
} prepared_call;

static const weivel_standard_described_type *type_named(const weivel_standard_description *description,
                                                        const char *name) {
    for (size_t index = 0; name != NULL && index < description->type_count; index++) {
        if (strcmp(description->types[index].name, name) == 0) {
            return &description->types[index];
        }
    }
    return NULL;
}

static const weivel_standard_described_operation *operation_named(const weivel_standard_description *description,
                                                                  const char *name) {
    for (size_t index = 0; name != NULL && index < description->operation_count; index++) {
        if (strcmp(description->operations[index].name, name) == 0) {
            return &description->operations[index];
        }
    }
    return NULL;
}

static const weivel_standard_described_member *member_named(const weivel_standard_described_type *type,
                                                            const char *name) {
    for (size_t index = 0; name != NULL && index < type->member_count; index++) {
        if (strcmp(type->members[index].name, name) == 0) {
            return &type->members[index];
        }
    }
    return NULL;
}

static int is_sized(const weivel_standard_described_type *type) {
    const weivel_standard_described_member *first = type->member_count == 0 ? NULL : &type->members[0];
    return first != NULL && strcmp(first->name, "size") == 0 && first->offset == 0 &&
           first->kind == WEIVEL_STANDARD_DESCRIBED_U32 && first->size == sizeof(uint32_t) &&
           type->first_shape > sizeof(uint32_t);
}

static int is_counted_array(uint32_t kind) {
    return kind == WEIVEL_STANDARD_DESCRIBED_TEXTS || kind == WEIVEL_STANDARD_DESCRIBED_BYTES ||
           kind == WEIVEL_STANDARD_DESCRIBED_ARRAY;
}

static parameter_rule struct_rule_of(uint32_t role) {
    switch (role) {
    case WEIVEL_STANDARD_DESCRIBED_INPUT:
        return RULE_STRUCT_INPUT;
    case WEIVEL_STANDARD_DESCRIBED_ANSWER:
        return RULE_STRUCT_ANSWER;
    case WEIVEL_STANDARD_DESCRIBED_REFUSAL:
        return RULE_REFUSAL;
    default:
        return RULE_UNREADABLE;
    }
}

static parameter_rule answer_rule_of(uint32_t kind) {
    switch (kind) {
    case WEIVEL_STANDARD_DESCRIBED_VALUE:
        return RULE_TREE_ANSWER;
    case WEIVEL_STANDARD_DESCRIBED_HANDLE:
    case WEIVEL_STANDARD_DESCRIBED_DEFINITION:
        return RULE_SLOT_ANSWER;
    default:
        return RULE_UNREADABLE;
    }
}

static parameter_rule rule_of(const weivel_standard_described_parameter *parameter) {
    if (parameter->kind == WEIVEL_STANDARD_DESCRIBED_CONTEXT) {
        return RULE_CONTEXT;
    }
    if (parameter->kind == WEIVEL_STANDARD_DESCRIBED_POINTER &&
        parameter->element == WEIVEL_STANDARD_DESCRIBED_STRUCT) {
        return struct_rule_of(parameter->role);
    }
    if (parameter->kind == WEIVEL_STANDARD_DESCRIBED_HANDLE && parameter->role == WEIVEL_STANDARD_DESCRIBED_INPUT) {
        return RULE_NULL_INPUT;
    }
    return parameter->role == WEIVEL_STANDARD_DESCRIBED_ANSWER ? answer_rule_of(parameter->kind) : RULE_UNREADABLE;
}

static int takes_struct(const weivel_standard_described_parameter *parameter) {
    parameter_rule rule = rule_of(parameter);
    return rule == RULE_STRUCT_INPUT || rule == RULE_STRUCT_ANSWER || rule == RULE_TREE_ANSWER || rule == RULE_REFUSAL;
}

/** A VALUE answer is weivel/value.h's tree, which no description lists among its types. */
static parameter_shape shape_of(const weivel_standard_description *description,
                                const weivel_standard_described_parameter *parameter) {
    parameter_shape shape = {0, 0};
    const weivel_standard_described_type *type = type_named(description, parameter->type);
    if (rule_of(parameter) == RULE_TREE_ANSWER) {
        shape.size = sizeof(weivel_standard_value_tree);
        shape.first_shape = offsetof(weivel_standard_value_tree, storage) + sizeof(void *);
    } else if (type != NULL && is_sized(type)) {
        shape.size = type->size;
        shape.first_shape = type->first_shape;
    }
    return shape;
}

static erased_function function_at(const definition_view *view, size_t offset) {
    erased_function function;
    memcpy(&function, (const unsigned char *)view->definition + offset, sizeof function);
    return function;
}

static void *context_of(const weivel_standard_description *description, const void *definition) {
    const weivel_standard_described_type *type = type_named(description, description->definition);
    void *context = NULL;
    for (size_t index = 0; type != NULL && index < type->member_count; index++) {
        if (type->members[index].kind == WEIVEL_STANDARD_DESCRIBED_CONTEXT) {
            memcpy(&context, (const unsigned char *)definition + type->members[index].offset, sizeof context);
        }
    }
    return context;
}

static void report(const definition_view *view, const weivel_standard_described_operation *operation,
                   const char *problem, const char *subject, spisor_check_tally *tally) {
    fprintf(stderr, "%s %s: %s%s%s\n", view->description->definition, operation->name, problem,
            subject == NULL ? "" : " ", subject == NULL ? "" : subject);
    tally->failures++;
}

/** Whether every parameter is one a rule here reads, so the operation can be called; reports the first that is not. */
static int is_callable(const definition_view *view, const weivel_standard_described_operation *operation,
                       spisor_check_tally *tally) {
    const weivel_standard_described_parameter *last;
    if (operation->parameter_count < 2 || operation->parameter_count - 1 > ARGUMENT_LIMIT ||
        rule_of(&operation->parameters[0]) != RULE_CONTEXT) {
        report(view, operation, "takes more parameters than this check calls, or no context first", NULL, tally);
        return 0;
    }
    for (size_t index = 1; index < operation->parameter_count; index++) {
        const weivel_standard_described_parameter *parameter = &operation->parameters[index];
        if (rule_of(parameter) == RULE_UNREADABLE || rule_of(parameter) == RULE_CONTEXT) {
            report(view, operation, "takes a parameter no rule here reads:", parameter->name, tally);
            return 0;
        }
        if (takes_struct(parameter) && shape_of(view->description, parameter).size == 0) {
            report(view, operation, "takes a struct that carries no `size`:", parameter->name, tally);
            return 0;
        }
    }
    last = &operation->parameters[operation->parameter_count - 1];
    if (rule_of(last) != RULE_REFUSAL || member_named(type_named(view->description, last->type), "detail") == NULL ||
        operation_named(view->description, last->release) == NULL) {
        report(view, operation, "takes no refusal last that names its detail and its release", NULL, tally);
        return 0;
    }
    return 1;
}

static void discard(prepared_call *call) {
    for (size_t index = 0; index < call->argument_count; index++) {
        free(call->buffers[index]);
        free(call->before[index]);
    }
}

static size_t buffer_size_of(const definition_view *view, const weivel_standard_described_parameter *parameter) {
    parameter_rule rule = rule_of(parameter);
    if (rule == RULE_SLOT_ANSWER) {
        return sizeof(void *);
    }
    return rule == RULE_NULL_INPUT ? 0 : shape_of(view->description, parameter).size;
}

static void set_size(unsigned char *at, size_t size) {
    uint32_t full = (uint32_t)size;
    memcpy(at, &full, sizeof full);
}

/** Sets the `size` of every size-prefixed struct `type` holds by value, at any depth, laid at `at`, in full. */
static void set_nested_sizes(const weivel_standard_description *description, const weivel_standard_described_type *type,
                             unsigned char *at) {
    for (size_t index = 0; type != NULL && index < type->member_count; index++) {
        const weivel_standard_described_member *member = &type->members[index];
        const weivel_standard_described_type *held = type_named(description, member->type);
        if (member->kind == WEIVEL_STANDARD_DESCRIBED_STRUCT && held != NULL) {
            if (is_sized(held)) {
                set_size(at + member->offset, held->size);
            }
            set_nested_sizes(description, held, at + member->offset);
        }
    }
}

/**
 * Zeroed buffers for every parameter after the context, each size-prefixed one, and each it holds by value, with its
 * `size` set in full.
 */
static int prepare(const definition_view *view, const weivel_standard_described_operation *operation,
                   prepared_call *call) {
    memset(call, 0, sizeof *call);
    call->argument_count = operation->parameter_count - 1;
    call->refusal_argument = call->argument_count - 1;
    for (size_t index = 0; index < call->argument_count; index++) {
        const weivel_standard_described_parameter *parameter = &operation->parameters[index + 1];
        size_t size = buffer_size_of(view, parameter);
        call->sizes[index] = size;
        call->answers[index] = parameter->role == WEIVEL_STANDARD_DESCRIBED_ANSWER;
        if (size == 0) {
            continue;
        }
        call->buffers[index] = calloc(1, size);
        call->before[index] = calloc(1, size);
        if (call->buffers[index] == NULL || call->before[index] == NULL) {
            discard(call);
            return 0;
        }
        if (takes_struct(parameter)) {
            set_size(call->buffers[index], size);
            set_nested_sizes(view->description, type_named(view->description, parameter->type), call->buffers[index]);
        }
        call->arguments[index] = call->buffers[index];
    }
    return 1;
}

static void apply(const definition_view *view, const weivel_standard_described_operation *operation,
                  const parameter_change *change, prepared_call *call) {
    unsigned char *buffer = call->buffers[change->parameter - 1];
    if (change->shortens) {
        parameter_shape shape = shape_of(view->description, &operation->parameters[change->parameter]);
        uint32_t short_size = (uint32_t)(shape.first_shape - 1);
        memcpy(buffer, &short_size, sizeof short_size);
    } else {
        size_t count = NONEMPTY_COUNT;
        memcpy(buffer + change->count_offset, &count, sizeof count);
    }
    for (size_t index = 0; index < call->argument_count; index++) {
        if (call->buffers[index] != NULL) {
            memcpy(call->before[index], call->buffers[index], call->sizes[index]);
        }
    }
}

SPISOR_CALLS_THROUGH_ERASED_TYPES
static weivel_status invoke(erased_function function, void *context, void *const *arguments, size_t count) {
    switch (count) {
    case 1:
        return ((operation_of_1)function)(context, arguments[0]);
    case 2:
        return ((operation_of_2)function)(context, arguments[0], arguments[1]);
    case 3:
        return ((operation_of_3)function)(context, arguments[0], arguments[1], arguments[2]);
    case 4:
        return ((operation_of_4)function)(context, arguments[0], arguments[1], arguments[2], arguments[3]);
    case 5:
        return ((operation_of_5)function)(context, arguments[0], arguments[1], arguments[2], arguments[3],
                                          arguments[4]);
    default:
        return ((operation_of_6)function)(context, arguments[0], arguments[1], arguments[2], arguments[3],
                                          arguments[4], arguments[5]);
    }
}

SPISOR_CALLS_THROUGH_ERASED_TYPES
static void release_refusal(const definition_view *view, const weivel_standard_described_operation *operation,
                            prepared_call *call) {
    const weivel_standard_described_parameter *refusal = &operation->parameters[operation->parameter_count - 1];
    const weivel_standard_described_operation *release = operation_named(view->description, refusal->release);
    ((release_function)function_at(view, release->offset))(view->context, call->buffers[call->refusal_argument]);
}

static int answers_unchanged(const prepared_call *call) {
    for (size_t index = 0; index < call->argument_count; index++) {
        if (call->answers[index] && call->buffers[index] != NULL &&
            memcmp(call->before[index], call->buffers[index], call->sizes[index]) != 0) {
            return 0;
        }
    }
    return 1;
}

static const char *refusal_detail(const definition_view *view, const weivel_standard_described_operation *operation,
                                  const prepared_call *call) {
    const weivel_standard_described_type *type =
        type_named(view->description, operation->parameters[operation->parameter_count - 1].type);
    const char *detail = NULL;
    memcpy(&detail, call->buffers[call->refusal_argument] + member_named(type, "detail")->offset, sizeof detail);
    return detail;
}

/** Calls `operation` with `change` made to its empty parameters and requires MALFORMED naming it, answers untouched. */
static void refuses_untouched(const definition_view *view, const weivel_standard_described_operation *operation,
                              const parameter_change *change, spisor_check_tally *tally) {
    prepared_call call;
    weivel_status status;
    const char *detail;
    tally->calls++;
    if (!prepare(view, operation, &call)) {
        report(view, operation, "could not be prepared: out of memory", NULL, tally);
        return;
    }
    apply(view, operation, change, &call);
    status = invoke(function_at(view, operation->offset), view->context, call.arguments, call.argument_count);
    detail = refusal_detail(view, operation, &call);
    if (status != WEIVEL_STATUS_MALFORMED || detail == NULL || strstr(detail, change->refused) == NULL ||
        !answers_unchanged(&call)) {
        fprintf(stderr, "%s %s %s: expected MALFORMED naming \"%s\" with every answer untouched; got %d, \"%s\"\n",
                view->description->definition, operation->name, change->label, change->refused, (int)status,
                detail == NULL ? "(none)" : detail);
        tally->failures++;
    }
    release_refusal(view, operation, &call);
    discard(&call);
}

static void short_shapes_of(const definition_view *view, const weivel_standard_described_operation *operation,
                            spisor_check_tally *tally) {
    for (size_t index = 1; index + 1 < operation->parameter_count; index++) {
        const weivel_standard_described_parameter *parameter = &operation->parameters[index];
        if (takes_struct(parameter)) {
            parameter_change change = {index, 0, 1, "short of its first shape", parameter->name};
            refuses_untouched(view, operation, &change, tally);
        }
    }
}

static int is_named_as_count(const weivel_standard_described_type *type, const char *name) {
    for (size_t index = 0; index < type->member_count; index++) {
        const weivel_standard_described_member *member = &type->members[index];
        if (is_counted_array(member->kind) && member->count != NULL && strcmp(member->count, name) == 0) {
            return 1;
        }
    }
    return 0;
}

/** One call per count of `type`, laid at `base` in the answer, then of every struct `type` holds by value. */
static void counts_of(const definition_view *view, const weivel_standard_described_operation *operation,
                      size_t parameter, const weivel_standard_described_type *type, size_t base, const char *path,
                      spisor_check_tally *tally) {
    for (size_t index = 0; index < type->member_count; index++) {
        const weivel_standard_described_member *member = &type->members[index];
        char label[LABEL_BYTES];
        snprintf(label, sizeof label, "%s.%s", path, member->name);
        if (member->kind == WEIVEL_STANDARD_DESCRIBED_STRUCT) {
            counts_of(view, operation, parameter, type_named(view->description, member->type), base + member->offset,
                      label, tally);
        } else if (member->kind == WEIVEL_STANDARD_DESCRIBED_SIZE && is_named_as_count(type, member->name)) {
            parameter_change change = {parameter, base + member->offset, 0, "not empty", label};
            if (member->size != sizeof(size_t)) {
                report(view, operation, "counts an array in a member that is no size_t:", label, tally);
                continue;
            }
            refuses_untouched(view, operation, &change, tally);
        }
    }
}

static void counted_answers_of(const definition_view *view, const weivel_standard_described_operation *operation,
                               spisor_check_tally *tally) {
    for (size_t index = 1; index + 1 < operation->parameter_count; index++) {
        const weivel_standard_described_parameter *parameter = &operation->parameters[index];
        if (rule_of(parameter) == RULE_STRUCT_ANSWER) {
            counts_of(view, operation, index, type_named(view->description, parameter->type), 0, parameter->type,
                      tally);
        }
    }
}

static int names_release(const weivel_standard_described_parameter *parameters, size_t parameter_count,
                         const char *name) {
    for (size_t index = 0; index < parameter_count; index++) {
        if (parameters[index].release != NULL && strcmp(parameters[index].release, name) == 0) {
            return 1;
        }
    }
    return 0;
}

static int is_release(const weivel_standard_description *description, const char *name) {
    for (size_t index = 0; index < description->operation_count; index++) {
        const weivel_standard_described_operation *operation = &description->operations[index];
        if (names_release(operation->parameters, operation->parameter_count, name)) {
            return 1;
        }
    }
    for (size_t type = 0; type < description->type_count; type++) {
        for (size_t index = 0; index < description->types[type].member_count; index++) {
            const char *release = description->types[type].members[index].release;
            if (release != NULL && strcmp(release, name) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

typedef void (*operation_check)(const definition_view *view, const weivel_standard_described_operation *operation,
                                spisor_check_tally *tally);

static void check_every_operation(const weivel_standard_description *description, const void *definition,
                                  operation_check check, spisor_check_tally *tally) {
    definition_view view = {description, definition, context_of(description, definition)};
    for (size_t index = 0; index < description->operation_count; index++) {
        const weivel_standard_described_operation *operation = &description->operations[index];
        if (operation->returns != WEIVEL_STANDARD_DESCRIBED_STATUS) {
            if (!is_release(description, operation->name)) {
                report(&view, operation, "answers no status and is no release a parameter or member names", NULL,
                       tally);
            }
        } else if (is_callable(&view, operation, tally)) {
            check(&view, operation, tally);
        }
    }
}

void spisor_check_short_shapes(const weivel_standard_description *description, const void *definition,
                               spisor_check_tally *tally) {
    size_t calls_before = tally->calls;
    check_every_operation(description, definition, short_shapes_of, tally);
    if (tally->calls == calls_before) {
        fprintf(stderr, "%s: no operation takes a size-prefixed struct, so nothing was checked\n",
                description->definition);
        tally->failures++;
    }
}

void spisor_check_counted_answers(const weivel_standard_description *description, const void *definition,
                                  spisor_check_tally *tally) {
    check_every_operation(description, definition, counted_answers_of, tally);
}

size_t spisor_check_described(const spisor_described_definition *definitions, size_t definition_count,
                              spisor_described_check check) {
    spisor_check_tally tally = {0, 0};
    for (size_t index = 0; index < definition_count; index++) {
        weivel_standard_refusal refusal = WEIVEL_INIT(weivel_standard_refusal);
        if (weivel_standard_description_check(definitions[index].description, &refusal) != WEIVEL_STATUS_OK) {
            fprintf(stderr, "description %zu is refused at \"%s\": %s\n", index,
                    refusal.path == NULL ? "" : refusal.path, refusal.detail == NULL ? "" : refusal.detail);
            weivel_standard_refusal_release(&refusal);
            tally.failures++;
            continue;
        }
        check(definitions[index].description, definitions[index].definition, &tally);
    }
    if (tally.calls == 0) {
        fprintf(stderr, "no operation of %zu definitions was called, so nothing was checked\n", definition_count);
        tally.failures++;
    }
    printf("%zu calls, %zu failures, over %zu definitions\n", tally.calls, tally.failures, definition_count);
    return tally.failures;
}
