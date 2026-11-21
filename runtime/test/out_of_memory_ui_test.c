#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "allocation.h"
#include "allocation_failure.h"
#include "check.h"
#include "definition_fixture.h"
#include "spisor/json_allocation.h"
#include "runtime/ui.h"

#define WRONG_RUN ((spisor_status)99)
#define PRINT_CAPACITY 8192
#define TREE_COUNT 16

static const char *const VIEWER =
    "[{\"id\": \"viewer\", \"views\": {\"overview\": {\"title\": \"Overview\", \"nodes\": ["
    "{\"id\": \"root\", \"component\": \"stack\"},"
    "{\"id\": \"rows\", \"component\": \"table\", \"parent\": \"root\", \"label\": \"Rows\","
    " \"bind\": {\"service\": \"viewer:status\", \"method\": \"rows\"}},"
    "{\"id\": \"load\", \"component\": \"chart\", \"chart\": \"line\", \"parent\": \"root\","
    " \"bind\": {\"service\": \"viewer:status\", \"method\": \"series\"}},"
    "{\"id\": \"broken\", \"component\": \"text\","
    " \"bind\": {\"service\": \"viewer:status\", \"method\": \"fails\"}},"
    "{\"id\": \"quiet\", \"component\": \"text\","
    " \"bind\": {\"service\": \"viewer:status\", \"method\": \"sleeps\"}}"
    "]}}, \"settings\": {\"depth\": {\"type\": \"number\", \"label\": \"Depth\"}, \"name\": {\"type\": \"string\"}},"
    " \"actions\": {\"sync\": {\"label\": \"Sync\", \"call\": {\"service\": \"viewer:main\", \"method\": \"sync\","
    " \"argument\": {\"force\": true, \"note\": \"declared\"}}}}}]";

/** One ui operation to fail allocation by allocation: `run` calls it over the inputs, printing what it answered. */
typedef struct operation_case {
    const char *label;
    spisor_status (*run)(void);
} operation_case;

/** A text an answer is printed into without allocating, so printing never moves the allocation count. */
typedef struct printed_answer {
    char text[PRINT_CAPACITY];
    size_t length;
} printed_answer;

/** What a clean run answered, which an answer under failure other than OUT_OF_MEMORY must equal. */
typedef struct clean_run {
    spisor_status status;
    char refusal[600];
    printed_answer printed;
    uint64_t allocations;
} clean_run;

static const runtime_ui_definition *ui;
static spisor_value_tree trees[TREE_COUNT];
static size_t tree_count = 0;
static char refusal_detail[600];
static printed_answer printed;

static const spisor_value *tree_of(const char *text) {
    spisor_value_tree *tree = &trees[tree_count++];
    tree->size = (uint32_t)sizeof *tree;
    CHECK(tree_count <= TREE_COUNT && value_of_text(text, tree));
    return &tree->root;
}

/** Every tree an operation reads, built before any allocation is failed. */
typedef struct operation_inputs {
    const spisor_value *viewer;
    const spisor_value *settings;
    const spisor_value *arguments[4];
    const spisor_value *rows;
    const spisor_value *series;
    const spisor_value *groups[3];
    const spisor_value *input;
} operation_inputs;

static operation_inputs inputs;

static void prepare(void) {
    inputs.viewer = tree_of(VIEWER);
    inputs.settings = tree_of("{\"theme\": \"dark\", \"plugins\": {\"viewer\": {\"pinned\": [\"overview\"]}}}");
    for (size_t index = 0; index < 4; index++) {
        inputs.arguments[index] = tree_of("{}");
    }
    inputs.rows = tree_of("{\"items\": [{\"id\": \"a\", \"state\": \"detected\", \"action\": \"open\"}]}");
    inputs.series = tree_of("{\"series\": 3, \"other\": null}");
    inputs.groups[0] = tree_of("[{\"id\": \"viewer\", \"label\": \"Viewer\"}]");
    inputs.groups[1] = tree_of("[{\"id\": \"greeter\"}]");
    inputs.groups[2] = tree_of("[{\"id\": \"viewer\"}, {\"id\": \"greeter\"}, {\"id\": \"rival\"}]");
    inputs.input = tree_of("{\"note\": \"n\", \"item\": \"typed\"}");
}

static void finish(void) {
    for (size_t index = 0; index < tree_count; index++) {
        spisor_value_release(&trees[index]);
    }
    tree_count = 0;
}

static void put(const char *text) {
    for (size_t index = 0; text != NULL && text[index] != '\0' && printed.length + 1 < PRINT_CAPACITY; index++) {
        printed.text[printed.length++] = text[index];
    }
    printed.text[printed.length] = '\0';
}

static void put_number(double number) {
    char text[40];
    snprintf(text, sizeof text, "%.17g", number);
    put(text);
}

static void put_count(size_t count) {
    put_number((double)count);
    put(";");
}

static void put_text(const char *text) {
    put(text == NULL ? "(none)" : text);
    put(";");
}

static void put_value(const spisor_value *value) {
    if (value == NULL) {
        put("(none)");
        return;
    }
    put_number((double)value->kind);
    put(":");
    if (value->kind == SPISOR_VALUE_BOOLEAN || value->kind == SPISOR_VALUE_NUMBER) {
        put_number(value->kind == SPISOR_VALUE_BOOLEAN ? (double)value->boolean : value->number);
    }
    put(value->kind == SPISOR_VALUE_TEXT ? value->text : "");
    put("[");
    for (size_t index = 0; value->kind == SPISOR_VALUE_LIST && index < value->count; index++) {
        put_value(&value->items[index]);
        put(",");
    }
    for (size_t index = 0; value->kind == SPISOR_VALUE_RECORD && index < value->count; index++) {
        put(value->entries[index].key);
        put("=");
        put_value(&value->entries[index].value);
        put(",");
    }
    put("]");
}

static void keep_refusal(spisor_refusal *refusal) {
    snprintf(refusal_detail, sizeof refusal_detail, "%s", refusal->detail == NULL ? "" : refusal->detail);
    ui->release_refusal(ui->context, refusal);
}

static spisor_status run_navigation(void) {
    runtime_ui_navigation_argument argument = SPISOR_INIT(runtime_ui_navigation_argument);
    runtime_ui_navigation_answer answer;
    start_empty(&answer, sizeof answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    argument.descriptions = inputs.viewer;
    argument.ui = inputs.settings;
    status = ui->navigation(ui->context, &argument, &answer, &refusal);
    for (size_t index = 0; index < answer.entry_count; index++) {
        put_text(answer.entries[index].plugin);
        put_count(answer.entries[index].kind);
        put_text(answer.entries[index].id);
        put_text(answer.entries[index].title);
        put_count(answer.entries[index].pinned);
    }
    put_value(answer.preferences);
    if (status == SPISOR_OK && answer.entry_count != 3) {
        status = WRONG_RUN;
    }
    status = empty_unless_answered(status, &answer, sizeof answer);
    ui->release_navigation_answer(ui->context, &answer);
    keep_refusal(&refusal);
    return status;
}

static spisor_status run_settings_view(void) {
    runtime_ui_settings_view_argument argument = SPISOR_INIT(runtime_ui_settings_view_argument);
    runtime_ui_settings_view_answer answer;
    start_empty(&answer, sizeof answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    argument.description = &inputs.viewer->items[0];
    status = ui->settings_view(ui->context, &argument, &answer, &refusal);
    put_text(answer.title);
    for (size_t index = 0; index < answer.node_count; index++) {
        put_text(answer.nodes[index].id);
        put_text(answer.nodes[index].parent);
        put_count(answer.nodes[index].component);
        put_text(answer.nodes[index].field);
        put_text(answer.nodes[index].label);
    }
    if (status == SPISOR_OK && answer.node_count != 3) {
        status = WRONG_RUN;
    }
    status = empty_unless_answered(status, &answer, sizeof answer);
    ui->release_settings_view_answer(ui->context, &answer);
    keep_refusal(&refusal);
    return status;
}

static const runtime_ui_entry OVERVIEW = {"viewer", RUNTIME_UI_KIND_VIEW, "overview", NULL, 0};
static const char *const SERIES[] = {"series"};

static runtime_ui_target target_of(const char *method, size_t index) {
    runtime_ui_target target = {"viewer", "viewer:status", method, NULL};
    target.arguments = inputs.arguments[index];
    return target;
}

static void put_check(const runtime_ui_check *check) {
    put_text(check->against);
    put_count(check->has_members);
    for (size_t index = 0; index < check->member_count; index++) {
        put_text(check->members[index]);
    }
}

static spisor_status plan_round(uint32_t round) {
    runtime_ui_screen_argument argument = SPISOR_INIT(runtime_ui_screen_argument);
    runtime_ui_plan_answer answer;
    start_empty(&answer, sizeof answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    runtime_ui_reply reply = {
        {NULL, NULL, NULL, NULL}, RUNTIME_UI_OUTCOME_UNAVAILABLE, NULL, NULL, 0, {NULL, NULL, NULL}};
    spisor_status status;
    reply.target = target_of("sleeps", 0);
    argument.descriptions = inputs.viewer;
    argument.screen = &OVERVIEW;
    argument.replies = &reply;
    argument.reply_count = 1;
    argument.round = round;
    status = ui->plan(ui->context, &argument, &answer, &refusal);
    for (size_t index = 0; index < answer.call_count; index++) {
        put_text(answer.calls[index].target.plugin);
        put_text(answer.calls[index].target.service);
        put_text(answer.calls[index].target.method);
        put_value(answer.calls[index].target.arguments);
        for (size_t check = 0; check < answer.calls[index].check_count; check++) {
            put_check(&answer.calls[index].checks[check]);
        }
    }
    if (status == SPISOR_OK && answer.call_count != 3) {
        status = WRONG_RUN;
    }
    status = empty_unless_answered(status, &answer, sizeof answer);
    ui->release_plan_answer(ui->context, &answer);
    keep_refusal(&refusal);
    return status;
}

static spisor_status run_plan(void) {
    return plan_round(15);
}

static spisor_status run_plan_past_its_rounds(void) {
    return plan_round(16);
}

static void put_screen(const runtime_ui_screen *screen) {
    put_text(screen->plugin);
    put_count(screen->kind);
    put_text(screen->view);
    put_text(screen->title);
    for (size_t index = 0; index < screen->node_count; index++) {
        const runtime_ui_node *node = &screen->nodes[index];
        put_text(node->id);
        put_text(node->component);
        put_text(node->parent);
        put_count(node->state);
        put_value(node->data);
        put_text(node->error == NULL ? NULL : node->error->detail);
        put_text(node->error == NULL ? NULL : node->error->fix);
        put_text(node->error == NULL ? NULL : node->error->path);
        put_value(node->props);
    }
}

/** The four replies the overview needs: a table answered, a chart whose check found an issue, a failure, a sleeper. */
static spisor_status render_with(size_t reply_count) {
    static const runtime_ui_issue issue = {"the series is no list", "answer a list", "series"};
    runtime_ui_checked table = {{"table", 0, NULL, 0}, NULL, 0};
    runtime_ui_checked chart = {{"chart", 1, SERIES, 1}, &issue, 1};
    runtime_ui_reply replies[4] = {
        {{NULL, NULL, NULL, NULL}, RUNTIME_UI_OUTCOME_ANSWERED, NULL, &table, 1, {NULL, NULL, NULL}},
        {{NULL, NULL, NULL, NULL}, RUNTIME_UI_OUTCOME_ANSWERED, NULL, &chart, 1, {NULL, NULL, NULL}},
        {{NULL, NULL, NULL, NULL}, RUNTIME_UI_OUTCOME_FAILED, NULL, NULL, 0, {"it threw", "fix it", NULL}},
        {{NULL, NULL, NULL, NULL}, RUNTIME_UI_OUTCOME_UNAVAILABLE, NULL, NULL, 0, {NULL, NULL, NULL}}};
    runtime_ui_screen_argument argument = SPISOR_INIT(runtime_ui_screen_argument);
    runtime_ui_screen answer;
    start_empty(&answer, sizeof answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    replies[0].target = target_of("rows", 0);
    replies[0].answer = inputs.rows;
    replies[1].target = target_of("series", 1);
    replies[1].answer = inputs.series;
    replies[2].target = target_of("fails", 2);
    replies[3].target = target_of("sleeps", 3);
    argument.descriptions = inputs.viewer;
    argument.screen = &OVERVIEW;
    argument.replies = replies;
    argument.reply_count = reply_count;
    status = ui->render(ui->context, &argument, &answer, &refusal);
    put_screen(&answer);
    if (status == SPISOR_OK && answer.node_count != 5) {
        status = WRONG_RUN;
    }
    status = empty_unless_answered(status, &answer, sizeof answer);
    ui->release_screen(ui->context, &answer);
    keep_refusal(&refusal);
    return status;
}

static spisor_status run_render(void) {
    return render_with(4);
}

static spisor_status run_render_refused(void) {
    return render_with(3);
}

static spisor_status run_plugin_list(void) {
    runtime_ui_plugin_list_argument argument = SPISOR_INIT(runtime_ui_plugin_list_argument);
    runtime_ui_plugin_list_answer answer;
    start_empty(&answer, sizeof answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    argument.installed = inputs.groups[0];
    argument.detected = inputs.groups[1];
    argument.available = inputs.groups[2];
    status = ui->plugin_list(ui->context, &argument, &answer, &refusal);
    put_text(answer.title);
    for (size_t index = 0; index < answer.row_count; index++) {
        put_text(answer.rows[index].id);
        put_count(answer.rows[index].state);
        put_count(answer.rows[index].unavailable);
        put_count(answer.rows[index].install);
        put_value(answer.rows[index].row);
    }
    if (status == SPISOR_OK && answer.row_count != 3) {
        status = WRONG_RUN;
    }
    status = empty_unless_answered(status, &answer, sizeof answer);
    ui->release_plugin_list_answer(ui->context, &answer);
    keep_refusal(&refusal);
    return status;
}

static spisor_status run_action(void) {
    static const char *const ITEMS[] = {"a", "b"};
    runtime_ui_action_argument argument = SPISOR_INIT(runtime_ui_action_argument);
    runtime_ui_action_answer answer;
    start_empty(&answer, sizeof answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    argument.description = &inputs.viewer->items[0];
    argument.action = "sync";
    argument.input = inputs.input;
    argument.item = "a";
    argument.has_items = 1;
    argument.items = ITEMS;
    argument.item_count = 2;
    status = ui->action(ui->context, &argument, &answer, &refusal);
    put_text(answer.target.plugin);
    put_text(answer.target.service);
    put_text(answer.target.method);
    put_value(answer.target.arguments);
    if (status == SPISOR_OK && (answer.target.arguments == NULL || answer.target.arguments->count != 4)) {
        status = WRONG_RUN;
    }
    status = empty_unless_answered(status, &answer, sizeof answer);
    ui->release_action_answer(ui->context, &answer);
    keep_refusal(&refusal);
    return status;
}

static void start_run(void) {
    printed.length = 0;
    printed.text[0] = '\0';
    refusal_detail[0] = '\0';
}

static void run_clean(const operation_case *each, clean_run *clean) {
    prepare();
    start_run();
    spisor_memory_fail_at(0);
    clean->status = each->run();
    clean->allocations = spisor_memory_attempts();
    snprintf(clean->refusal, sizeof clean->refusal, "%s", refusal_detail);
    clean->printed = printed;
    finish();
}

/** Whether an answer other than OUT_OF_MEMORY is the clean run's, its refusal and every field it filled alike. */
static int matches_clean(const clean_run *clean, spisor_status status, const char *label, uint64_t attempt) {
    int same_answer = printed.length == clean->printed.length && strcmp(printed.text, clean->printed.text) == 0;
    if (status == clean->status && strcmp(refusal_detail, clean->refusal) == 0 && same_answer) {
        return 1;
    }
    fprintf(stderr, "%s: failing allocation %llu answered %d \"%s\"%s; a clean run answers %d \"%s\"\n", label,
            (unsigned long long)attempt, (int)status, refusal_detail, same_answer ? "" : " filling another answer",
            (int)clean->status, clean->refusal);
    return 0;
}

static allocation_failure_mode failure_mode = FAIL_ONE_ALLOCATION;

/** Fails allocation attempt; 1 to go on, 0 after a failure the case does not allow, and *ran_whole when none failed. */
static int fail_once(const operation_case *each, const clean_run *clean, uint64_t attempt, int *ran_whole) {
    int64_t baseline;
    spisor_status status;
    int kept;
    prepare();
    baseline = spisor_memory_live_blocks();
    start_run();
    fail_allocation(failure_mode, attempt);
    status = each->run();
    *ran_whole = !spisor_memory_failed();
    spisor_memory_fail_at(0);
    kept = spisor_memory_live_blocks() == baseline;
    if (!kept) {
        fprintf(stderr, "%s: failing allocation %llu left %lld blocks live\n", each->label,
                (unsigned long long)attempt, (long long)(spisor_memory_live_blocks() - baseline));
    }
    if (kept && (status != SPISOR_OUT_OF_MEMORY || *ran_whole)) {
        kept = matches_clean(clean, status, each->label, attempt);
    }
    finish();
    return kept;
}

/** Fails the operation's allocations one at a time, between first and last when given, until it runs whole. */
static void every_failure_in_mode(const operation_case *each, spisor_status expected, uint64_t first, uint64_t last) {
    clean_run clean;
    uint64_t attempt = first;
    int ran_whole = 0;
    run_clean(each, &clean);
    if (clean.status != expected) {
        fprintf(stderr, "%s: a clean run answered %d \"%s\"\n", each->label, (int)clean.status, clean.refusal);
        check_failures++;
    }
    for (; check_failures == 0 && !ran_whole && (last == 0 || attempt <= last); attempt++) {
        if (!fail_once(each, &clean, attempt, &ran_whole)) {
            check_failures++;
        }
    }
    printf("%s%s: %llu allocations, failures %llu to %llu checked\n", each->label,
           allocation_failure_mode_name(failure_mode), (unsigned long long)clean.allocations,
           (unsigned long long)first, (unsigned long long)(attempt - 1));
}

static void every_failure_of(const operation_case *each, spisor_status expected, uint64_t first, uint64_t last) {
    for (failure_mode = FAIL_ONE_ALLOCATION; failure_mode < ALLOCATION_FAILURE_MODES; failure_mode++) {
        every_failure_in_mode(each, expected, first, last);
    }
}

static int is_selected(const char *only, const char *label) {
    return strncmp(label, only, strlen(only)) == 0;
}

/** With a label's start as the first argument runs only the cases it starts, failing allocations first to last. */
int main(int argc, char **argv) {
    static const operation_case cases[8] = {{"ui's navigation", run_navigation},
                                            {"ui's settings_view", run_settings_view},
                                            {"ui's plan", run_plan},
                                            {"ui's render", run_render},
                                            {"ui's render refused", run_render_refused},
                                            {"ui's plugin_list", run_plugin_list},
                                            {"ui's action", run_action},
                                            {"ui's rounds refused", run_plan_past_its_rounds}};
    const char *only = argc > 1 ? argv[1] : "";
    uint64_t first = argc > 2 ? strtoull(argv[2], NULL, 10) : 1;
    uint64_t last = argc > 3 ? strtoull(argv[3], NULL, 10) : 0;
    spisor_json_use_library_allocator();
    ui = runtime_ui_definition_of();
    for (size_t index = 0; index < 8; index++) {
        spisor_status expected = index == 4 || index == 7 ? SPISOR_REFUSED : SPISOR_OK;
        if (is_selected(only, cases[index].label) && (index != 3 || strcmp(only, cases[4].label) != 0)) {
            every_failure_of(&cases[index], expected, first == 0 ? 1 : first, last);
        }
    }
    return check_summary("out_of_memory_ui_test");
}
