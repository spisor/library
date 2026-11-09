#include <stdlib.h>
#include <string.h>

#include "catalog_call.h"
#include "check.h"
#include "entry_template.h"

static const runtime_entry_values VALUES = {
    "/h", "/s/entries/l", "sample-loader", "/c/boot/stub", "sample-host", "/c/hint.json", "/c/bundles"};

static void expect_expansion(const char *template_text, const char *expected, const char *label) {
    char *expanded = runtime_entry_expand(template_text, &VALUES);
    check_text(expanded, expected, label);
    free(expanded);
}

static void replaces_every_occurrence(void) {
    expect_expansion("{{home}}/a/{{home}}", "/h/a//h", "a placeholder used twice");
}

static void replaces_each_of_the_closed_set(void) {
    expect_expansion("{{home}} {{entryRoot}} {{loaderId}} {{boot}} {{bootedHost}} {{hint}} {{bundles}}",
                     "/h /s/entries/l sample-loader /c/boot/stub sample-host /c/hint.json /c/bundles",
                     "every placeholder");
}

static void replaces_a_placeholder_touching_literal_text(void) {
    expect_expansion("spisor-{{loaderId}}.js", "spisor-sample-loader.js", "a placeholder inside a name");
}

static void writes_the_json_form_as_a_string_literal(void) {
    runtime_entry_values quoted = VALUES;
    quoted.hint = "C:\\cache\\\"hint\".json";
    char *expanded = runtime_entry_expand("const HINT = {{json:hint}};", &quoted);
    check_text(expanded, "const HINT = \"C:\\\\cache\\\\\\\"hint\\\".json\";", "a value needing escapes");
    free(expanded);
}

static void leaves_a_javascript_template_literal_alone(void) {
    expect_expansion("`${x}` {{home}} ${y}", "`${x}` /h ${y}", "a template literal beside a placeholder");
}

static void copies_text_with_no_placeholder(void) {
    expect_expansion("plain { text } }}", "plain { text } }}", "braces that open nothing");
}

static void knows_the_closed_set_and_refuses_the_rest(void) {
    CHECK(runtime_entry_template_is_known("{{home}}/x {{json:boot}} ${a} plain"));
    CHECK(runtime_entry_template_is_known(""));
    CHECK(!runtime_entry_template_is_known("{{nonsense}}"));
    CHECK(!runtime_entry_template_is_known("{{json:nonsense}}"));
    CHECK(!runtime_entry_template_is_known("{{home}} {{unclosed"));
    CHECK(!runtime_entry_template_is_known("{{Home}}"));
}

int main(void) {
    replaces_every_occurrence();
    replaces_each_of_the_closed_set();
    replaces_a_placeholder_touching_literal_text();
    writes_the_json_form_as_a_string_literal();
    leaves_a_javascript_template_literal_alone();
    copies_text_with_no_placeholder();
    knows_the_closed_set_and_refuses_the_rest();
    return check_summary("entry_template_test");
}
