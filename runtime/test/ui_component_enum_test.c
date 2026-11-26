#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "spisor/allocation.h"
#include "spisor/file.h"
#include "ui_components.h"

/**
 * @file
 * Reads `runtime_ui_component` out of ui.h with the strict form a renderer's build reads it with, so a line that form
 * cannot read fails here first: the enum opens and closes on lines of their own, and between them every value is
 * `    RUNTIME_UI_COMPONENT_<UPPER_SNAKE> = <decimal>,` under a one-line doc comment. Requires the thirty components of
 * the 2026-11-02 design's vocabulary in its order, numbered from 1 without a gap, each the one ui's render knows by its
 * lower snake case name.
 */

#define ENUM_OPENING "typedef enum runtime_ui_component {"
#define ENUM_CLOSING "} runtime_ui_component;"
#define VALUE_PREFIX "    RUNTIME_UI_COMPONENT_"
#define DOC_PREFIX "    /** "
#define DOC_SUFFIX " */"
#define MOST_COMPONENTS 64
#define LONGEST_NAME 64

static const char *const VOCABULARY[] = {
    "STACK", "GRID",  "SPLIT", "TABS",       "SECTION",    "CARD",   "DIVIDER", "TEXT",   "LIST",
    "TABLE", "TREE",  "KEY_VALUE", "CHART",  "LOG_STREAM", "CODE_BLOCK", "DIFF", "BADGE",   "ICON",
    "FIELD", "SEARCH", "FORM", "COMMAND_ENTRY", "CONFIRMATION", "TOAST", "PROGRESS", "SPINNER",
    "EMPTY", "LOADING", "ERROR", "UNAVAILABLE"};

#define VOCABULARY_COUNT (sizeof VOCABULARY / sizeof VOCABULARY[0])

typedef struct read_component {
    char name[LONGEST_NAME];
    long value;
} read_component;

typedef struct read_enum {
    read_component components[MOST_COMPONENTS];
    size_t count;
    int failed;
} read_enum;

static void refuse_line(read_enum *read, const char *line, const char *why) {
    fprintf(stderr, "ui.h: %s: \"%s\"\n", why, line);
    read->failed = 1;
}

static int has_prefix(const char *text, const char *prefix) {
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static int is_doc_line(const char *line) {
    size_t length = strlen(line);
    return has_prefix(line, DOC_PREFIX) && length > strlen(DOC_PREFIX) + strlen(DOC_SUFFIX) &&
           strcmp(line + length - strlen(DOC_SUFFIX), DOC_SUFFIX) == 0 && strstr(line + strlen(DOC_PREFIX), "*/") ==
                                                                              line + length - strlen(DOC_SUFFIX) + 1;
}

/** The name and value of one value line, 0 for a line not of exactly the form. */
static int read_value_line(const char *line, read_component *component) {
    const char *at = line + strlen(VALUE_PREFIX);
    size_t length = 0;
    if (!has_prefix(line, VALUE_PREFIX) || !isupper((unsigned char)*at)) {
        return 0;
    }
    while (isupper((unsigned char)at[length]) || isdigit((unsigned char)at[length]) || at[length] == '_') {
        length++;
    }
    if (length >= LONGEST_NAME || at[length - 1] == '_' || strncmp(at + length, " = ", 3) != 0) {
        return 0;
    }
    memcpy(component->name, at, length);
    component->name[length] = '\0';
    at += length + 3;
    if (!isdigit((unsigned char)*at) || (*at == '0' && at[1] != ',')) {
        return 0;
    }
    component->value = strtol(at, (char **)&at, 10);
    return strcmp(at, ",") == 0;
}

/** Reads one line inside the enum, which alternates a doc comment and the value it documents. */
static void read_inside(read_enum *read, const char *line, int *documented) {
    if (is_doc_line(line) && !*documented) {
        *documented = 1;
    } else if (!*documented) {
        refuse_line(read, line, "a line where a value's one-line doc comment belongs");
    } else if (read->count == MOST_COMPONENTS || !read_value_line(line, &read->components[read->count])) {
        refuse_line(read, line, "a line under a doc comment that is no value line of the enum's form");
    } else {
        read->count++;
        *documented = 0;
    }
}

static void read_lines(read_enum *read, char *text) {
    int inside = 0;
    int closed = 0;
    int documented = 0;
    for (char *line = strtok(text, "\n"); line != NULL && !closed; line = strtok(NULL, "\n")) {
        size_t length = strlen(line);
        if (length > 0 && line[length - 1] == '\r') {
            line[length - 1] = '\0';
        }
        if (!inside) {
            inside = strcmp(line, ENUM_OPENING) == 0;
        } else if (strcmp(line, ENUM_CLOSING) == 0) {
            closed = 1;
        } else {
            read_inside(read, line, &documented);
        }
    }
    if (!closed || documented) {
        fprintf(stderr, "ui.h: the enum runtime_ui_component %s\n", closed ? "ends on a doc comment" : "is not there");
        read->failed = 1;
    }
}

static void read_enum_of_header(read_enum *read) {
    size_t length = 0;
    spisor_read_outcome outcome;
    char *text = (char *)spisor_read_file_reporting(RUNTIME_UI_HEADER_PATH, &length, &outcome);
    memset(read, 0, sizeof *read);
    if (text == NULL) {
        fprintf(stderr, "cannot read %s\n", RUNTIME_UI_HEADER_PATH);
        read->failed = 1;
        return;
    }
    read_lines(read, text);
    spisor_memory_release(text);
}

static void lower_case(const char *name, char *lowered) {
    size_t index = 0;
    for (; name[index] != '\0'; index++) {
        lowered[index] = (char)tolower((unsigned char)name[index]);
    }
    lowered[index] = '\0';
}

static void reads_thirty_components_numbered_from_one_in_the_vocabularys_order(void) {
    read_enum read;
    read_enum_of_header(&read);
    CHECK(!read.failed);
    CHECK(read.count == VOCABULARY_COUNT);
    for (size_t index = 0; index < read.count && index < VOCABULARY_COUNT; index++) {
        char lowered[LONGEST_NAME];
        check_text(read.components[index].name, VOCABULARY[index], "a component in the vocabulary's order");
        if (read.components[index].value != (long)index + 1) {
            fprintf(stderr, "%s is %ld, not %zu\n", read.components[index].name, read.components[index].value,
                    index + 1);
            check_failures++;
        }
        lower_case(read.components[index].name, lowered);
        if (runtime_ui_component_of(lowered) != (uint32_t)read.components[index].value) {
            fprintf(stderr, "render does not know %s as %ld\n", lowered, read.components[index].value);
            check_failures++;
        }
    }
}

static void knows_no_name_beside_the_enums(void) {
    const char *const strangers[] = {"blink", "", "Text", "TEXT", "key-value", "keyvalue", "text ", "form2"};
    for (size_t index = 0; index < sizeof strangers / sizeof strangers[0]; index++) {
        if (runtime_ui_component_of(strangers[index]) != 0) {
            fprintf(stderr, "render knows \"%s\"\n", strangers[index]);
            check_failures++;
        }
    }
    CHECK(runtime_ui_component_of(NULL) == 0);
}

int main(void) {
    reads_thirty_components_numbered_from_one_in_the_vocabularys_order();
    knows_no_name_beside_the_enums();
    return check_summary("ui_component_enum_test");
}
