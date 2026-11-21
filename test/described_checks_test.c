#include <stddef.h>
#include <stdint.h>

#include "check.h"
#include "described_checks.h"

/**
 * @file
 * Runs the described checks over a definition described by hand, whose operations keep or break the rules on demand,
 * so each check is shown to fail on the defect it names, every kind of parameter it reads is exercised, and each
 * operation it cannot call is reported rather than skipped.
 */

typedef struct probe_handle probe_handle;

struct probe_handle {
    int unused;
};

typedef struct probe_refusal {
    uint32_t size;
    const char *detail;
    const char *fix;
    void *storage;
} probe_refusal;

typedef struct probe_answer {
    uint32_t size;
    const char *const *names;
    size_t name_count;
    void *storage;
} probe_answer;

typedef struct probe_argument {
    uint32_t size;
    const char *label;
} probe_argument;

typedef struct probe_list {
    uint32_t size;
    const char *const *names;
    size_t name_count;
} probe_list;

typedef struct probe_nested_answer {
    uint32_t size;
    probe_list list;
    void *storage;
} probe_nested_answer;

typedef struct probe_definition {
    uint32_t size;
    void *context;
    weivel_status (*fill)(void *context, probe_answer *answer, probe_refusal *refusal);
    weivel_status (*fill_nested)(void *context, probe_nested_answer *answer, probe_refusal *refusal);
    weivel_status (*use_handle)(void *context, probe_handle *handle, probe_answer *answer, probe_refusal *refusal);
    weivel_status (*open)(void *context, const probe_argument *argument, probe_handle **handle, probe_refusal *refusal);
    weivel_status (*take_number)(void *context, uint32_t number, probe_refusal *refusal);
    void (*forget)(void *context, probe_answer *answer, probe_answer *other);
    void (*release_answer)(void *context, probe_answer *answer);
    void (*release_nested)(void *context, probe_nested_answer *answer);
    void (*release_handle)(void *context, probe_handle *handle);
    void (*release_refusal)(void *context, probe_refusal *refusal);
} probe_definition;

typedef enum probe_behaviour {
    PROBE_KEEPS_THE_RULES,
    PROBE_FILLS_A_SHORT_ANSWER,
    PROBE_WRITES_AS_IT_REFUSES,
    PROBE_IGNORES_THE_HELD_COUNT
} probe_behaviour;

static probe_behaviour behaviour = PROBE_KEEPS_THE_RULES;
static const char *const probe_names[] = {"one"};
static probe_handle the_handle;
static size_t handle_calls = 0;
static const probe_handle *handle_seen = NULL;

static weivel_status refuse(probe_refusal *refusal, const char *detail) {
    refusal->detail = detail;
    return WEIVEL_STATUS_MALFORMED;
}

static weivel_status probe_fill(void *context, probe_answer *answer, probe_refusal *refusal) {
    (void)context;
    if (behaviour == PROBE_WRITES_AS_IT_REFUSES) {
        answer->names = probe_names;
    }
    if (answer->size < offsetof(probe_answer, storage) + sizeof answer->storage) {
        return behaviour == PROBE_FILLS_A_SHORT_ANSWER ? WEIVEL_STATUS_OK : refuse(refusal, "short of its first shape");
    }
    if (answer->name_count != 0) {
        return refuse(refusal, "the answer is not empty");
    }
    answer->names = probe_names;
    answer->name_count = 1;
    return WEIVEL_STATUS_OK;
}

/** Reads the list's own `size` before its count, as a provider checking every size it is handed would. */
static weivel_status probe_fill_nested(void *context, probe_nested_answer *answer, probe_refusal *refusal) {
    (void)context;
    if (answer->size < offsetof(probe_nested_answer, storage) + sizeof answer->storage) {
        return refuse(refusal, "short of its first shape");
    }
    if (answer->list.size < sizeof answer->list) {
        return refuse(refusal, "the list is too small");
    }
    if (behaviour != PROBE_IGNORES_THE_HELD_COUNT && answer->list.name_count != 0) {
        return refuse(refusal, "the answer is not empty");
    }
    answer->list.names = probe_names;
    answer->list.name_count++;
    return WEIVEL_STATUS_OK;
}

static weivel_status probe_use_handle(void *context, probe_handle *handle, probe_answer *answer,
                                      probe_refusal *refusal) {
    handle_calls++;
    handle_seen = handle;
    return probe_fill(context, answer, refusal);
}

static weivel_status probe_open(void *context, const probe_argument *argument, probe_handle **handle,
                                probe_refusal *refusal) {
    (void)context;
    if (behaviour == PROBE_WRITES_AS_IT_REFUSES) {
        *handle = &the_handle;
    }
    if (argument->size < offsetof(probe_argument, label) + sizeof argument->label) {
        return refuse(refusal, "short of its first shape");
    }
    *handle = &the_handle;
    return WEIVEL_STATUS_OK;
}

static weivel_status probe_take_number(void *context, uint32_t number, probe_refusal *refusal) {
    (void)context;
    (void)number;
    return refuse(refusal, "short of its first shape");
}

static void probe_forget(void *context, probe_answer *answer, probe_answer *other) {
    (void)context;
    (void)answer;
    (void)other;
}

static void probe_release_answer(void *context, probe_answer *answer) {
    (void)context;
    (void)answer;
}

static void probe_release_nested(void *context, probe_nested_answer *answer) {
    (void)context;
    (void)answer;
}

static void probe_release_handle(void *context, probe_handle *handle) {
    (void)context;
    (void)handle;
}

static void probe_release_refusal(void *context, probe_refusal *refusal) {
    (void)context;
    refusal->detail = NULL;
}

static const probe_definition probe = {sizeof(probe_definition), NULL,
                                       probe_fill, probe_fill_nested,
                                       probe_use_handle, probe_open,
                                       probe_take_number, probe_forget,
                                       probe_release_answer, probe_release_nested,
                                       probe_release_handle, probe_release_refusal};

#define MEMBER(owner, field, member_kind, held, counted_by)                                                     \
    {.name = #field, .offset = offsetof(owner, field), .size = sizeof(((owner *)0)->field), .kind = (member_kind), \
     .element = WEIVEL_STANDARD_DESCRIBED_NONE, .type = (held), .count = (counted_by), .release = NULL}

static const weivel_standard_described_member refusal_members[] = {
    MEMBER(probe_refusal, size, WEIVEL_STANDARD_DESCRIBED_U32, NULL, NULL),
    MEMBER(probe_refusal, detail, WEIVEL_STANDARD_DESCRIBED_TEXT, NULL, NULL),
    MEMBER(probe_refusal, fix, WEIVEL_STANDARD_DESCRIBED_TEXT, NULL, NULL),
    MEMBER(probe_refusal, storage, WEIVEL_STANDARD_DESCRIBED_STORAGE, NULL, NULL)};

static const weivel_standard_described_member answer_members[] = {
    MEMBER(probe_answer, size, WEIVEL_STANDARD_DESCRIBED_U32, NULL, NULL),
    MEMBER(probe_answer, names, WEIVEL_STANDARD_DESCRIBED_TEXTS, NULL, "name_count"),
    MEMBER(probe_answer, name_count, WEIVEL_STANDARD_DESCRIBED_SIZE, NULL, NULL),
    MEMBER(probe_answer, storage, WEIVEL_STANDARD_DESCRIBED_STORAGE, NULL, NULL)};

static const weivel_standard_described_member argument_members[] = {
    MEMBER(probe_argument, size, WEIVEL_STANDARD_DESCRIBED_U32, NULL, NULL),
    MEMBER(probe_argument, label, WEIVEL_STANDARD_DESCRIBED_TEXT, NULL, NULL)};

static const weivel_standard_described_member list_members[] = {
    MEMBER(probe_list, size, WEIVEL_STANDARD_DESCRIBED_U32, NULL, NULL),
    MEMBER(probe_list, names, WEIVEL_STANDARD_DESCRIBED_TEXTS, NULL, "name_count"),
    MEMBER(probe_list, name_count, WEIVEL_STANDARD_DESCRIBED_SIZE, NULL, NULL)};

static const weivel_standard_described_member nested_answer_members[] = {
    MEMBER(probe_nested_answer, size, WEIVEL_STANDARD_DESCRIBED_U32, NULL, NULL),
    MEMBER(probe_nested_answer, list, WEIVEL_STANDARD_DESCRIBED_STRUCT, "probe_list", NULL),
    MEMBER(probe_nested_answer, storage, WEIVEL_STANDARD_DESCRIBED_STORAGE, NULL, NULL)};

static const weivel_standard_described_member definition_members[] = {
    MEMBER(probe_definition, size, WEIVEL_STANDARD_DESCRIBED_U32, NULL, NULL),
    MEMBER(probe_definition, context, WEIVEL_STANDARD_DESCRIBED_CONTEXT, NULL, NULL)};

#define TYPE(owner, member_array, through)                                                                        \
    {.name = #owner, .size = sizeof(owner), .first_shape = offsetof(owner, through) + sizeof(((owner *)0)->through), \
     .members = (member_array), .member_count = sizeof(member_array) / sizeof((member_array)[0])}

static const weivel_standard_described_type probe_types[] = {
    TYPE(probe_refusal, refusal_members, storage),
    TYPE(probe_answer, answer_members, storage),
    TYPE(probe_argument, argument_members, label),
    TYPE(probe_list, list_members, name_count),
    TYPE(probe_nested_answer, nested_answer_members, storage),
    TYPE(probe_definition, definition_members, release_refusal)};

#define PARAMETER(parameter_name, parameter_kind, parameter_role, parameter_type, parameter_release)                  \
    {.name = (parameter_name), .kind = (parameter_kind), .role = (parameter_role),                                   \
     .element = (parameter_kind) == WEIVEL_STANDARD_DESCRIBED_POINTER ? WEIVEL_STANDARD_DESCRIBED_STRUCT              \
                                                                      : WEIVEL_STANDARD_DESCRIBED_NONE,              \
     .type = (parameter_type), .count = NULL, .release = (parameter_release)}

#define CONTEXT_PARAMETER \
    PARAMETER("context", WEIVEL_STANDARD_DESCRIBED_CONTEXT, WEIVEL_STANDARD_DESCRIBED_INPUT, NULL, NULL)
#define REFUSAL_PARAMETER                                                                                   \
    PARAMETER("refusal", WEIVEL_STANDARD_DESCRIBED_POINTER, WEIVEL_STANDARD_DESCRIBED_REFUSAL, "probe_refusal", \
              "release_refusal")
#define ANSWER_PARAMETER(type, release) \
    PARAMETER("answer", WEIVEL_STANDARD_DESCRIBED_POINTER, WEIVEL_STANDARD_DESCRIBED_ANSWER, type, release)
#define HANDLE_PARAMETER(role)                                                                            \
    PARAMETER("handle", WEIVEL_STANDARD_DESCRIBED_HANDLE, WEIVEL_STANDARD_DESCRIBED_##role, "probe_handle", \
              "release_handle")

static const weivel_standard_described_parameter fill_parameters[] = {
    CONTEXT_PARAMETER, ANSWER_PARAMETER("probe_answer", "release_answer"), REFUSAL_PARAMETER};

static const weivel_standard_described_parameter fill_nested_parameters[] = {
    CONTEXT_PARAMETER, ANSWER_PARAMETER("probe_nested_answer", "release_nested"), REFUSAL_PARAMETER};

static const weivel_standard_described_parameter use_handle_parameters[] = {
    CONTEXT_PARAMETER, HANDLE_PARAMETER(INPUT), ANSWER_PARAMETER("probe_answer", "release_answer"),
    REFUSAL_PARAMETER};

static const weivel_standard_described_parameter open_parameters[] = {
    CONTEXT_PARAMETER,
    PARAMETER("argument", WEIVEL_STANDARD_DESCRIBED_POINTER, WEIVEL_STANDARD_DESCRIBED_INPUT, "probe_argument", NULL),
    HANDLE_PARAMETER(ANSWER), REFUSAL_PARAMETER};

static const weivel_standard_described_parameter take_number_parameters[] = {
    CONTEXT_PARAMETER,
    PARAMETER("number", WEIVEL_STANDARD_DESCRIBED_U32, WEIVEL_STANDARD_DESCRIBED_INPUT, NULL, NULL),
    REFUSAL_PARAMETER};

static const weivel_standard_described_parameter forget_parameters[] = {
    CONTEXT_PARAMETER, ANSWER_PARAMETER("probe_answer", NULL),
    PARAMETER("other", WEIVEL_STANDARD_DESCRIBED_POINTER, WEIVEL_STANDARD_DESCRIBED_ANSWER, "probe_answer", NULL)};

static const weivel_standard_described_parameter release_answer_parameters[] = {
    CONTEXT_PARAMETER, ANSWER_PARAMETER("probe_answer", NULL)};

static const weivel_standard_described_parameter release_nested_parameters[] = {
    CONTEXT_PARAMETER, ANSWER_PARAMETER("probe_nested_answer", NULL)};

static const weivel_standard_described_parameter release_handle_parameters[] = {CONTEXT_PARAMETER,
                                                                                HANDLE_PARAMETER(INPUT)};

static const weivel_standard_described_parameter release_refusal_parameters[] = {
    CONTEXT_PARAMETER,
    PARAMETER("refusal", WEIVEL_STANDARD_DESCRIBED_POINTER, WEIVEL_STANDARD_DESCRIBED_REFUSAL, "probe_refusal", NULL)};

#define OPERATION(field, form, parameter_array)                                                              \
    {.name = #field, .offset = offsetof(probe_definition, field), .returns = (form), .parameters = (parameter_array), \
     .parameter_count = sizeof(parameter_array) / sizeof((parameter_array)[0])}
#define STATUS_OPERATION(field) OPERATION(field, WEIVEL_STANDARD_DESCRIBED_STATUS, field##_parameters)
#define RELEASE_OPERATION(field) OPERATION(field, WEIVEL_STANDARD_DESCRIBED_NOTHING, field##_parameters)

static const weivel_standard_described_operation plain_operations[] = {
    STATUS_OPERATION(fill), RELEASE_OPERATION(release_answer), RELEASE_OPERATION(release_refusal)};

static const weivel_standard_described_operation nested_operations[] = {
    STATUS_OPERATION(fill_nested), RELEASE_OPERATION(release_nested), RELEASE_OPERATION(release_refusal)};

static const weivel_standard_described_operation handle_input_operations[] = {
    STATUS_OPERATION(use_handle), RELEASE_OPERATION(release_answer), RELEASE_OPERATION(release_handle),
    RELEASE_OPERATION(release_refusal)};

static const weivel_standard_described_operation handle_answer_operations[] = {
    STATUS_OPERATION(open), RELEASE_OPERATION(release_handle), RELEASE_OPERATION(release_refusal)};

static const weivel_standard_described_operation unreadable_operations[] = {
    STATUS_OPERATION(fill), STATUS_OPERATION(take_number), RELEASE_OPERATION(release_answer),
    RELEASE_OPERATION(release_refusal)};

static const weivel_standard_described_operation stray_operations[] = {
    STATUS_OPERATION(fill), RELEASE_OPERATION(forget), RELEASE_OPERATION(release_answer),
    RELEASE_OPERATION(release_refusal)};

#define SCENARIO(operation_array) described(operation_array, sizeof(operation_array) / sizeof((operation_array)[0]))

static weivel_standard_description described(const weivel_standard_described_operation *operations,
                                             size_t operation_count) {
    weivel_standard_description description = {(uint32_t)sizeof(weivel_standard_description),
                                               "probe_definition",
                                               probe_types,
                                               sizeof probe_types / sizeof probe_types[0],
                                               NULL,
                                               0,
                                               operations,
                                               operation_count,
                                               NULL,
                                               0,
                                               NULL,
                                               0};
    return description;
}

static int passes_the_standards_check(const weivel_standard_description *description) {
    weivel_standard_refusal refusal = WEIVEL_INIT(weivel_standard_refusal);
    weivel_status status = weivel_standard_description_check(description, &refusal);
    if (status != WEIVEL_STATUS_OK) {
        fprintf(stderr, "the probe's description is refused at \"%s\": %s\n", refusal.path == NULL ? "" : refusal.path,
                refusal.detail == NULL ? "" : refusal.detail);
        weivel_standard_refusal_release(&refusal);
    }
    return status == WEIVEL_STATUS_OK;
}

static spisor_check_tally run(const weivel_standard_description *description, spisor_described_check check,
                              probe_behaviour chosen) {
    spisor_check_tally tally = {0, 0};
    behaviour = chosen;
    check(description, &probe, &tally);
    behaviour = PROBE_KEEPS_THE_RULES;
    return tally;
}

static void a_definition_keeping_the_rules_passes_both_checks(void) {
    weivel_standard_description description = SCENARIO(plain_operations);
    spisor_described_definition definition = {&description, &probe};
    spisor_check_tally shapes = run(&description, spisor_check_short_shapes, PROBE_KEEPS_THE_RULES);
    spisor_check_tally counts = run(&description, spisor_check_counted_answers, PROBE_KEEPS_THE_RULES);
    CHECK(passes_the_standards_check(&description));
    CHECK(shapes.calls == 1 && shapes.failures == 0);
    CHECK(counts.calls == 1 && counts.failures == 0);
    CHECK(spisor_check_described(&definition, 1, spisor_check_short_shapes) == 0);
}

static void filling_a_short_answer_fails_the_short_shape_check(void) {
    weivel_standard_description description = SCENARIO(plain_operations);
    spisor_check_tally tally = run(&description, spisor_check_short_shapes, PROBE_FILLS_A_SHORT_ANSWER);
    CHECK(tally.calls == 1 && tally.failures == 1);
}

static void writing_into_a_refused_answer_fails_both_checks(void) {
    weivel_standard_description description = SCENARIO(plain_operations);
    CHECK(run(&description, spisor_check_short_shapes, PROBE_WRITES_AS_IT_REFUSES).failures == 1);
    CHECK(run(&description, spisor_check_counted_answers, PROBE_WRITES_AS_IT_REFUSES).failures == 1);
}

static void a_count_inside_a_struct_held_by_value_is_checked(void) {
    weivel_standard_description description = SCENARIO(nested_operations);
    spisor_check_tally kept = run(&description, spisor_check_counted_answers, PROBE_KEEPS_THE_RULES);
    spisor_check_tally ignored = run(&description, spisor_check_counted_answers, PROBE_IGNORES_THE_HELD_COUNT);
    CHECK(passes_the_standards_check(&description));
    CHECK(kept.calls == 1 && kept.failures == 0);
    CHECK(ignored.calls == 1 && ignored.failures == 1);
}

static void a_struct_held_by_value_has_its_own_size_set_in_full(void) {
    weivel_standard_description description = SCENARIO(nested_operations);
    spisor_check_tally shapes = run(&description, spisor_check_short_shapes, PROBE_KEEPS_THE_RULES);
    spisor_check_tally counts = run(&description, spisor_check_counted_answers, PROBE_KEEPS_THE_RULES);
    CHECK(shapes.calls == 1 && shapes.failures == 0);
    CHECK(counts.calls == 1 && counts.failures == 0);
}

static void a_handle_input_is_passed_null(void) {
    weivel_standard_description description = SCENARIO(handle_input_operations);
    spisor_check_tally tally;
    handle_calls = 0;
    handle_seen = &the_handle;
    tally = run(&description, spisor_check_short_shapes, PROBE_KEEPS_THE_RULES);
    CHECK(passes_the_standards_check(&description));
    CHECK(tally.calls == 1 && tally.failures == 0);
    CHECK(handle_calls == 1 && handle_seen == NULL);
}

static void a_handle_answer_written_while_refusing_fails(void) {
    weivel_standard_description description = SCENARIO(handle_answer_operations);
    spisor_check_tally kept = run(&description, spisor_check_short_shapes, PROBE_KEEPS_THE_RULES);
    spisor_check_tally written = run(&description, spisor_check_short_shapes, PROBE_WRITES_AS_IT_REFUSES);
    CHECK(passes_the_standards_check(&description));
    CHECK(kept.calls == 1 && kept.failures == 0);
    CHECK(written.calls == 1 && written.failures == 1);
}

static void a_parameter_no_rule_reads_is_a_failure(void) {
    weivel_standard_description description = SCENARIO(unreadable_operations);
    spisor_check_tally tally = run(&description, spisor_check_short_shapes, PROBE_KEEPS_THE_RULES);
    CHECK(passes_the_standards_check(&description));
    CHECK(tally.calls == 1 && tally.failures == 1);
}

static void an_operation_answering_no_status_that_is_no_release_is_a_failure(void) {
    weivel_standard_description description = SCENARIO(stray_operations);
    spisor_check_tally tally = run(&description, spisor_check_short_shapes, PROBE_KEEPS_THE_RULES);
    CHECK(tally.calls == 1 && tally.failures == 1);
}

static void a_description_yielding_no_operation_fails_rather_than_passes(void) {
    weivel_standard_description description = described(plain_operations, 0);
    spisor_described_definition definition = {&description, &probe};
    spisor_check_tally tally = {0, 0};
    spisor_check_short_shapes(&description, &probe, &tally);
    CHECK(tally.calls == 0 && tally.failures == 1);
    CHECK(spisor_check_described(&definition, 1, spisor_check_counted_answers) == 1);
    CHECK(spisor_check_described(NULL, 0, spisor_check_short_shapes) == 1);
}

int main(void) {
    a_definition_keeping_the_rules_passes_both_checks();
    filling_a_short_answer_fails_the_short_shape_check();
    writing_into_a_refused_answer_fails_both_checks();
    a_count_inside_a_struct_held_by_value_is_checked();
    a_struct_held_by_value_has_its_own_size_set_in_full();
    a_handle_input_is_passed_null();
    a_handle_answer_written_while_refusing_fails();
    a_parameter_no_rule_reads_is_a_failure();
    an_operation_answering_no_status_that_is_no_release_is_a_failure();
    a_description_yielding_no_operation_fails_rather_than_passes();
    return check_summary("described_checks_test");
}
