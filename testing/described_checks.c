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

/**
 * @implNote Every argument but a double is passed as a 64-bit word, which every ABI CI builds for (x86-64 System V,
 * Windows x64, AAPCS64 with Apple's variant) passes exactly as it passes a pointer or a 64-bit integer, and as it
 * passes a narrower integer, whose callee reads the low bytes: one general register or one 8-byte stack slot. Only
 * a double takes another register file, so a call is typed by which of its positions hold doubles.
 */
_Static_assert(sizeof(void *) == sizeof(uint64_t) && sizeof(size_t) == sizeof(uint64_t),
               "the described checks pass a pointer or a size_t as a 64-bit word, so they build for 64-bit targets");

typedef void (*erased_function)(void);
typedef void (*release_function)(void *, void *);

typedef enum parameter_rule {
    RULE_UNREADABLE,
    RULE_CONTEXT,
    RULE_STRUCT_INPUT,
    RULE_STRUCT_ANSWER,
    RULE_TREE_ANSWER,
    RULE_NULL_INPUT,
    RULE_SCALAR_INPUT,
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

/**
 * One argument per parameter after the context, in the order the operation takes them: a word, or a double where
 * `doubles` has its bit set, and the buffer a pointer argument points at.
 */
typedef struct prepared_call {
    uint64_t words[ARGUMENT_LIMIT];
    double reals[ARGUMENT_LIMIT];
    unsigned doubles;
    unsigned char *buffers[ARGUMENT_LIMIT];
    unsigned char *before[ARGUMENT_LIMIT];
    size_t sizes[ARGUMENT_LIMIT];
    int answers[ARGUMENT_LIMIT];
    size_t argument_count;
    int refuses;
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

static const weivel_standard_described_enum *enum_named(const weivel_standard_description *description,
                                                        const char *name) {
    for (size_t index = 0; name != NULL && index < description->enum_count; index++) {
        if (strcmp(description->enums[index].name, name) == 0) {
            return &description->enums[index];
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
           first->kind == WEIVEL_STANDARD_DESCRIBED_U32 && first->size == sizeof(uint32_t);
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

static int is_scalar(uint32_t kind) {
    return kind == WEIVEL_STANDARD_DESCRIBED_U32 || kind == WEIVEL_STANDARD_DESCRIBED_U64 ||
           kind == WEIVEL_STANDARD_DESCRIBED_I64 || kind == WEIVEL_STANDARD_DESCRIBED_F64 ||
           kind == WEIVEL_STANDARD_DESCRIBED_SIZE;
}

/** The bytes an answer slot of `kind` holds: the scalar itself, or the pointer a text, handle or definition is. */
static size_t slot_size_of(uint32_t kind) {
    switch (kind) {
    case WEIVEL_STANDARD_DESCRIBED_U32:
        return sizeof(uint32_t);
    case WEIVEL_STANDARD_DESCRIBED_U64:
    case WEIVEL_STANDARD_DESCRIBED_I64:
        return sizeof(uint64_t);
    case WEIVEL_STANDARD_DESCRIBED_F64:
        return sizeof(double);
    case WEIVEL_STANDARD_DESCRIBED_SIZE:
        return sizeof(size_t);
    default:
        return sizeof(void *);
    }
}

static parameter_rule answer_rule_of(uint32_t kind) {
    if (kind == WEIVEL_STANDARD_DESCRIBED_VALUE) {
        return RULE_TREE_ANSWER;
    }
    if (is_scalar(kind) || kind == WEIVEL_STANDARD_DESCRIBED_TEXT || kind == WEIVEL_STANDARD_DESCRIBED_HANDLE ||
        kind == WEIVEL_STANDARD_DESCRIBED_DEFINITION) {
        return RULE_SLOT_ANSWER;
    }
    return RULE_UNREADABLE;
}

/** An input passed NULL, with any count of it 0: a handle, a text, texts, bytes, an array, or a value node. */
static int is_null_input(const weivel_standard_described_parameter *parameter) {
    switch (parameter->kind) {
    case WEIVEL_STANDARD_DESCRIBED_HANDLE:
    case WEIVEL_STANDARD_DESCRIBED_TEXT:
    case WEIVEL_STANDARD_DESCRIBED_TEXTS:
    case WEIVEL_STANDARD_DESCRIBED_BYTES:
    case WEIVEL_STANDARD_DESCRIBED_ARRAY:
        return 1;
    case WEIVEL_STANDARD_DESCRIBED_POINTER:
        return parameter->element == WEIVEL_STANDARD_DESCRIBED_VALUE;
    default:
        return 0;
    }
}

static parameter_rule input_rule_of(const weivel_standard_described_parameter *parameter) {
    if (is_scalar(parameter->kind)) {
        return RULE_SCALAR_INPUT;
    }
    return is_null_input(parameter) ? RULE_NULL_INPUT : RULE_UNREADABLE;
}

static parameter_rule rule_of(const weivel_standard_described_parameter *parameter) {
    if (parameter->kind == WEIVEL_STANDARD_DESCRIBED_CONTEXT) {
        return RULE_CONTEXT;
    }
    if (parameter->kind == WEIVEL_STANDARD_DESCRIBED_POINTER &&
        parameter->element == WEIVEL_STANDARD_DESCRIBED_STRUCT) {
        return struct_rule_of(parameter->role);
    }
    switch (parameter->role) {
    case WEIVEL_STANDARD_DESCRIBED_INPUT:
        return input_rule_of(parameter);
    case WEIVEL_STANDARD_DESCRIBED_ANSWER:
        return answer_rule_of(parameter->kind);
    default:
        return RULE_UNREADABLE;
    }
}

/**
 * What a scalar input is passed: an enum's first listed value that is not 0, since the standard keeps 0 for none,
 * as weivel_standard_log_level does, or 0 when it lists no other; and 0 for any other scalar, which is a count of
 * nothing beside the NULL input it counts, no flag, or zero.
 */
static uint64_t default_word_of(const weivel_standard_description *description,
                                const weivel_standard_described_parameter *parameter) {
    const weivel_standard_described_enum *named =
        parameter->kind == WEIVEL_STANDARD_DESCRIBED_U32 ? enum_named(description, parameter->type) : NULL;
    for (size_t index = 0; named != NULL && index < named->entry_count; index++) {
        if (named->entries[index].number != 0) {
            return named->entries[index].number;
        }
    }
    return 0;
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

/** rule_of, with an input struct that carries no `size` passed NULL, since no shape of it can be shortened. */
static parameter_rule rule_in(const weivel_standard_description *description,
                              const weivel_standard_described_parameter *parameter) {
    parameter_rule rule = rule_of(parameter);
    return rule == RULE_STRUCT_INPUT && shape_of(description, parameter).size == 0 ? RULE_NULL_INPUT : rule;
}

static int takes_struct(const weivel_standard_description *description,
                        const weivel_standard_described_parameter *parameter) {
    parameter_rule rule = rule_in(description, parameter);
    return rule == RULE_STRUCT_INPUT || rule == RULE_STRUCT_ANSWER || rule == RULE_TREE_ANSWER || rule == RULE_REFUSAL;
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

static int takes_refusal(const weivel_standard_described_operation *operation) {
    return rule_of(&operation->parameters[operation->parameter_count - 1]) == RULE_REFUSAL;
}

/**
 * Whether every parameter is one a rule here reads, so the operation can be called; reports the first that is not.
 *
 * @implNote Strict, and per operation: the operations it can read are still checked, but one it cannot is a failure,
 * since every kind a description publishes is read here, so an unreadable one is a kind appended since or more
 * parameters than a call here passes, and either needs this file changed, which a passing note would hide.
 */
static int is_callable(const definition_view *view, const weivel_standard_described_operation *operation,
                       spisor_check_tally *tally) {
    const weivel_standard_described_parameter *last;
    if (operation->parameter_count < 1 || operation->parameter_count - 1 > ARGUMENT_LIMIT ||
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
        if (takes_struct(view->description, parameter) && shape_of(view->description, parameter).size == 0) {
            report(view, operation, "takes a struct that carries no `size`:", parameter->name, tally);
            return 0;
        }
    }
    last = &operation->parameters[operation->parameter_count - 1];
    if (takes_refusal(operation) && (member_named(type_named(view->description, last->type), "detail") == NULL ||
                                     operation_named(view->description, last->release) == NULL)) {
        report(view, operation, "takes a refusal that names no detail or no release", NULL, tally);
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
    parameter_rule rule = rule_in(view->description, parameter);
    if (rule == RULE_SLOT_ANSWER) {
        return slot_size_of(parameter->kind);
    }
    return rule == RULE_NULL_INPUT || rule == RULE_SCALAR_INPUT ? 0 : shape_of(view->description, parameter).size;
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

static void pass_scalar(const definition_view *view, const weivel_standard_described_parameter *parameter,
                        size_t index, prepared_call *call) {
    if (parameter->kind == WEIVEL_STANDARD_DESCRIBED_F64) {
        call->reals[index] = 0.0;
        call->doubles |= 1u << index;
    } else {
        call->words[index] = default_word_of(view->description, parameter);
    }
}

/**
 * Zeroed buffers for every pointer parameter after the context, each size-prefixed one, and each it holds by value,
 * with its `size` set in full; NULL for an input passed NULL, and its default for a scalar.
 */
static int prepare(const definition_view *view, const weivel_standard_described_operation *operation,
                   prepared_call *call) {
    memset(call, 0, sizeof *call);
    call->argument_count = operation->parameter_count - 1;
    call->refuses = takes_refusal(operation);
    for (size_t index = 0; index < call->argument_count; index++) {
        const weivel_standard_described_parameter *parameter = &operation->parameters[index + 1];
        size_t size = buffer_size_of(view, parameter);
        call->sizes[index] = size;
        call->answers[index] = parameter->role == WEIVEL_STANDARD_DESCRIBED_ANSWER;
        if (rule_in(view->description, parameter) == RULE_SCALAR_INPUT) {
            pass_scalar(view, parameter, index, call);
            continue;
        }
        if (size == 0) {
            continue;
        }
        call->buffers[index] = calloc(1, size);
        call->before[index] = calloc(1, size);
        if (call->buffers[index] == NULL || call->before[index] == NULL) {
            discard(call);
            return 0;
        }
        if (takes_struct(view->description, parameter)) {
            set_size(call->buffers[index], size);
            set_nested_sizes(view->description, type_named(view->description, parameter->type), call->buffers[index]);
        }
        call->words[index] = (uint64_t)(uintptr_t)call->buffers[index];
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

/*
 * CHOOSE_<n>(index, doubles, types, values) expands to one `case` per way the n arguments from `index` on can each be
 * a word or a double, the case being the `doubles` bits, and each calls the operation through the type those slots
 * make, `types` and `values` the parameter types and arguments chosen before it. No variadic list is forwarded to
 * another macro, which MSVC's traditional preprocessor would pass as a single argument.
 */
#define SLOT_TYPE_0 uint64_t
#define SLOT_TYPE_1 double
#define SLOT_VALUE_0(index) call->words[index]
#define SLOT_VALUE_1(index) call->reals[index]
#define UNPACK(...) __VA_ARGS__
#define CALL_SHAPE(doubles, types, values) \
    case doubles:                          \
        return ((weivel_status(*)(UNPACK types))function)(UNPACK values);
#define WORD_AT(types) (UNPACK types, SLOT_TYPE_0)
#define DOUBLE_AT(types) (UNPACK types, SLOT_TYPE_1)
#define WORD_OF(index, values) (UNPACK values, SLOT_VALUE_0(index))
#define DOUBLE_OF(index, values) (UNPACK values, SLOT_VALUE_1(index))
#define AS_DOUBLE(index, doubles) ((doubles) | 1u << (index))
#define CHOOSE_0(index, doubles, types, values) CALL_SHAPE(doubles, types, values)
#define CHOOSE_1(index, doubles, types, values)                                                \
    CHOOSE_0((index) + 1, doubles, WORD_AT(types), WORD_OF(index, values))              \
    CHOOSE_0((index) + 1, AS_DOUBLE(index, doubles), DOUBLE_AT(types), DOUBLE_OF(index, values))
#define CHOOSE_2(index, doubles, types, values)                                                \
    CHOOSE_1((index) + 1, doubles, WORD_AT(types), WORD_OF(index, values))              \
    CHOOSE_1((index) + 1, AS_DOUBLE(index, doubles), DOUBLE_AT(types), DOUBLE_OF(index, values))
#define CHOOSE_3(index, doubles, types, values)                                                \
    CHOOSE_2((index) + 1, doubles, WORD_AT(types), WORD_OF(index, values))              \
    CHOOSE_2((index) + 1, AS_DOUBLE(index, doubles), DOUBLE_AT(types), DOUBLE_OF(index, values))
#define CHOOSE_4(index, doubles, types, values)                                                \
    CHOOSE_3((index) + 1, doubles, WORD_AT(types), WORD_OF(index, values))              \
    CHOOSE_3((index) + 1, AS_DOUBLE(index, doubles), DOUBLE_AT(types), DOUBLE_OF(index, values))
#define CHOOSE_5(index, doubles, types, values)                                                \
    CHOOSE_4((index) + 1, doubles, WORD_AT(types), WORD_OF(index, values))              \
    CHOOSE_4((index) + 1, AS_DOUBLE(index, doubles), DOUBLE_AT(types), DOUBLE_OF(index, values))
#define CHOOSE_6(index, doubles, types, values)                                                \
    CHOOSE_5((index) + 1, doubles, WORD_AT(types), WORD_OF(index, values))              \
    CHOOSE_5((index) + 1, AS_DOUBLE(index, doubles), DOUBLE_AT(types), DOUBLE_OF(index, values))
#define CALLS_OF(count)                                                \
    case count:                                                        \
        switch (call->doubles) { CHOOSE_##count(0, 0u, (void *), (context)) } \
        break;

/** Calls `function` through the type its arguments make, slot by slot; ARGUMENT_LIMIT bounds the count. */
SPISOR_CALLS_THROUGH_ERASED_TYPES
static weivel_status invoke(erased_function function, void *context, const prepared_call *call) {
    switch (call->argument_count) {
        CALLS_OF(0)
        CALLS_OF(1)
        CALLS_OF(2)
        CALLS_OF(3)
        CALLS_OF(4)
        CALLS_OF(5)
        CALLS_OF(6)
    }
    return WEIVEL_STATUS_OK;
}

SPISOR_CALLS_THROUGH_ERASED_TYPES
static void release_refusal(const definition_view *view, const weivel_standard_described_operation *operation,
                            prepared_call *call) {
    const weivel_standard_described_parameter *refusal = &operation->parameters[operation->parameter_count - 1];
    const weivel_standard_described_operation *release = operation_named(view->description, refusal->release);
    ((release_function)function_at(view, release->offset))(view->context, call->buffers[call->argument_count - 1]);
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
    memcpy(&detail, call->buffers[call->argument_count - 1] + member_named(type, "detail")->offset, sizeof detail);
    return detail;
}

/** Whether the call refused as `change` requires: MALFORMED, and a detail naming the change when it takes a refusal. */
static int refused_as_required(const parameter_change *change, weivel_status status, const char *detail,
                               int refuses) {
    return status == WEIVEL_STATUS_MALFORMED &&
           (!refuses || (detail != NULL && strstr(detail, change->refused) != NULL));
}

/**
 * Calls `operation` with `change` made to its empty parameters and requires MALFORMED naming it, answers untouched;
 * an operation taking no refusal has no detail to name it in, so only its status and its answers are read.
 */
static void refuses_untouched(const definition_view *view, const weivel_standard_described_operation *operation,
                              const parameter_change *change, spisor_check_tally *tally) {
    prepared_call call;
    weivel_status status;
    const char *detail = NULL;
    tally->calls++;
    if (!prepare(view, operation, &call)) {
        report(view, operation, "could not be prepared: out of memory", NULL, tally);
        return;
    }
    apply(view, operation, change, &call);
    status = invoke(function_at(view, operation->offset), view->context, &call);
    if (call.refuses) {
        detail = refusal_detail(view, operation, &call);
    }
    if (!refused_as_required(change, status, detail, call.refuses) || !answers_unchanged(&call)) {
        fprintf(stderr, "%s %s %s: expected MALFORMED naming \"%s\" with every answer untouched; got %d, \"%s\"\n",
                view->description->definition, operation->name, change->label, change->refused, (int)status,
                detail == NULL ? "(none)" : detail);
        tally->failures++;
    }
    if (call.refuses) {
        release_refusal(view, operation, &call);
    }
    discard(&call);
}

static void short_shapes_of(const definition_view *view, const weivel_standard_described_operation *operation,
                            spisor_check_tally *tally) {
    for (size_t index = 1; index < operation->parameter_count; index++) {
        const weivel_standard_described_parameter *parameter = &operation->parameters[index];
        if (takes_struct(view->description, parameter) && rule_of(parameter) != RULE_REFUSAL) {
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
    for (size_t index = 1; index < operation->parameter_count; index++) {
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

static int answers_a_status(const weivel_standard_description *description) {
    for (size_t index = 0; index < description->operation_count; index++) {
        if (description->operations[index].returns == WEIVEL_STANDARD_DESCRIBED_STATUS) {
            return 1;
        }
    }
    return 0;
}

void spisor_check_short_shapes(const weivel_standard_description *description, const void *definition,
                               spisor_check_tally *tally) {
    check_every_operation(description, definition, short_shapes_of, tally);
    if (!answers_a_status(description)) {
        fprintf(stderr, "%s: no operation answers a status, so nothing was checked\n", description->definition);
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
