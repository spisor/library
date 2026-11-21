#include <stddef.h>
#include <stdint.h>

#include "check.h"
#include "described_checks.h"

/**
 * @file
 * Runs the described checks over a definition described by hand, whose one operation keeps or breaks the rules on
 * demand, so each check is shown to fail on the defect it names, and to fail rather than pass on a description that
 * gives it nothing to call.
 */

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

typedef struct probe_definition {
    uint32_t size;
    void *context;
    weivel_status (*fill)(void *context, probe_answer *answer, probe_refusal *refusal);
    void (*release_answer)(void *context, probe_answer *answer);
    void (*release_refusal)(void *context, probe_refusal *refusal);
} probe_definition;

typedef enum probe_behaviour {
    PROBE_KEEPS_THE_RULES,
    PROBE_FILLS_A_SHORT_ANSWER,
    PROBE_WRITES_AS_IT_REFUSES
} probe_behaviour;

static probe_behaviour behaviour = PROBE_KEEPS_THE_RULES;
static const char *const probe_names[] = {"one"};

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

static void probe_release_answer(void *context, probe_answer *answer) {
    (void)context;
    (void)answer;
}

static void probe_release_refusal(void *context, probe_refusal *refusal) {
    (void)context;
    refusal->detail = NULL;
}

static const probe_definition probe = {sizeof(probe_definition), NULL, probe_fill, probe_release_answer,
                                       probe_release_refusal};

#define MEMBER(owner, field, member_kind, counted_by)                                                           \
    {.name = #field, .offset = offsetof(owner, field), .size = sizeof(((owner *)0)->field), .kind = (member_kind), \
     .element = WEIVEL_STANDARD_DESCRIBED_NONE, .type = NULL, .count = (counted_by), .release = NULL}

static const weivel_standard_described_member refusal_members[] = {
    MEMBER(probe_refusal, size, WEIVEL_STANDARD_DESCRIBED_U32, NULL),
    MEMBER(probe_refusal, detail, WEIVEL_STANDARD_DESCRIBED_TEXT, NULL),
    MEMBER(probe_refusal, fix, WEIVEL_STANDARD_DESCRIBED_TEXT, NULL),
    MEMBER(probe_refusal, storage, WEIVEL_STANDARD_DESCRIBED_STORAGE, NULL)};

static const weivel_standard_described_member answer_members[] = {
    MEMBER(probe_answer, size, WEIVEL_STANDARD_DESCRIBED_U32, NULL),
    MEMBER(probe_answer, names, WEIVEL_STANDARD_DESCRIBED_TEXTS, "name_count"),
    MEMBER(probe_answer, name_count, WEIVEL_STANDARD_DESCRIBED_SIZE, NULL),
    MEMBER(probe_answer, storage, WEIVEL_STANDARD_DESCRIBED_STORAGE, NULL)};

static const weivel_standard_described_member definition_members[] = {
    MEMBER(probe_definition, size, WEIVEL_STANDARD_DESCRIBED_U32, NULL),
    MEMBER(probe_definition, context, WEIVEL_STANDARD_DESCRIBED_CONTEXT, NULL)};

#define TYPE(owner, member_array, through)                                                                        \
    {.name = #owner, .size = sizeof(owner), .first_shape = offsetof(owner, through) + sizeof(((owner *)0)->through), \
     .members = (member_array), .member_count = sizeof(member_array) / sizeof((member_array)[0])}

static const weivel_standard_described_type probe_types[] = {
    TYPE(probe_refusal, refusal_members, storage), TYPE(probe_answer, answer_members, storage),
    TYPE(probe_definition, definition_members, release_refusal)};

#define PARAMETER(parameter_name, parameter_kind, parameter_role, parameter_type, parameter_release)                  \
    {.name = (parameter_name), .kind = (parameter_kind), .role = (parameter_role),                                   \
     .element = (parameter_kind) == WEIVEL_STANDARD_DESCRIBED_POINTER ? WEIVEL_STANDARD_DESCRIBED_STRUCT              \
                                                                      : WEIVEL_STANDARD_DESCRIBED_NONE,              \
     .type = (parameter_type), .count = NULL, .release = (parameter_release)}

#define CONTEXT_PARAMETER \
    PARAMETER("context", WEIVEL_STANDARD_DESCRIBED_CONTEXT, WEIVEL_STANDARD_DESCRIBED_INPUT, NULL, NULL)

static const weivel_standard_described_parameter fill_parameters[] = {
    CONTEXT_PARAMETER,
    PARAMETER("answer", WEIVEL_STANDARD_DESCRIBED_POINTER, WEIVEL_STANDARD_DESCRIBED_ANSWER, "probe_answer",
              "release_answer"),
    PARAMETER("refusal", WEIVEL_STANDARD_DESCRIBED_POINTER, WEIVEL_STANDARD_DESCRIBED_REFUSAL, "probe_refusal",
              "release_refusal")};

static const weivel_standard_described_parameter release_answer_parameters[] = {
    CONTEXT_PARAMETER,
    PARAMETER("answer", WEIVEL_STANDARD_DESCRIBED_POINTER, WEIVEL_STANDARD_DESCRIBED_ANSWER, "probe_answer", NULL)};

static const weivel_standard_described_parameter release_refusal_parameters[] = {
    CONTEXT_PARAMETER,
    PARAMETER("refusal", WEIVEL_STANDARD_DESCRIBED_POINTER, WEIVEL_STANDARD_DESCRIBED_REFUSAL, "probe_refusal", NULL)};

#define OPERATION(field, form, parameter_array)                                                              \
    {.name = #field, .offset = offsetof(probe_definition, field), .returns = (form), .parameters = (parameter_array), \
     .parameter_count = sizeof(parameter_array) / sizeof((parameter_array)[0])}

static const weivel_standard_described_operation probe_operations[] = {
    OPERATION(fill, WEIVEL_STANDARD_DESCRIBED_STATUS, fill_parameters),
    OPERATION(release_answer, WEIVEL_STANDARD_DESCRIBED_NOTHING, release_answer_parameters),
    OPERATION(release_refusal, WEIVEL_STANDARD_DESCRIBED_NOTHING, release_refusal_parameters)};

static weivel_standard_description probe_description(size_t operation_count) {
    weivel_standard_description description = {(uint32_t)sizeof(weivel_standard_description),
                                               "probe_definition",
                                               probe_types,
                                               sizeof probe_types / sizeof probe_types[0],
                                               NULL,
                                               0,
                                               probe_operations,
                                               operation_count,
                                               NULL,
                                               0,
                                               NULL,
                                               0};
    return description;
}

static spisor_check_tally run(spisor_described_check check, probe_behaviour chosen) {
    weivel_standard_description description = probe_description(sizeof probe_operations / sizeof probe_operations[0]);
    spisor_check_tally tally = {0, 0};
    behaviour = chosen;
    check(&description, &probe, &tally);
    behaviour = PROBE_KEEPS_THE_RULES;
    return tally;
}

static void a_definition_keeping_the_rules_passes_both_checks(void) {
    weivel_standard_description description = probe_description(sizeof probe_operations / sizeof probe_operations[0]);
    spisor_described_definition definition = {&description, &probe};
    spisor_check_tally shapes = run(spisor_check_short_shapes, PROBE_KEEPS_THE_RULES);
    spisor_check_tally counts = run(spisor_check_counted_answers, PROBE_KEEPS_THE_RULES);
    CHECK(shapes.calls == 1 && shapes.failures == 0);
    CHECK(counts.calls == 1 && counts.failures == 0);
    CHECK(spisor_check_described(&definition, 1, spisor_check_short_shapes) == 0);
}

static void filling_a_short_answer_fails_the_short_shape_check(void) {
    spisor_check_tally tally = run(spisor_check_short_shapes, PROBE_FILLS_A_SHORT_ANSWER);
    CHECK(tally.calls == 1 && tally.failures == 1);
}

static void writing_into_a_refused_answer_fails_both_checks(void) {
    CHECK(run(spisor_check_short_shapes, PROBE_WRITES_AS_IT_REFUSES).failures == 1);
    CHECK(run(spisor_check_counted_answers, PROBE_WRITES_AS_IT_REFUSES).failures == 1);
}

static void a_description_yielding_no_operation_fails_rather_than_passes(void) {
    weivel_standard_description description = probe_description(0);
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
    a_description_yielding_no_operation_fails_rather_than_passes();
    return check_summary("described_checks_test");
}
