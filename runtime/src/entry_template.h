#ifndef RUNTIME_ENTRY_TEMPLATE_H
#define RUNTIME_ENTRY_TEMPLATE_H

/** What the placeholders of an application's declared entry stand for. */
typedef struct runtime_entry_values {
    const char *home;
    const char *entry_root;
    const char *loader_id;
    const char *boot;
    const char *booted_host;
    const char *hint;
    const char *bundles;
} runtime_entry_values;

/**
 * template_text with each {{name}} replaced by its value and each {{json:name}} by that value as a JSON
 * string literal. Freed with free; NULL when memory runs out. An unknown placeholder is copied as written,
 * since loader.check refuses one before a loader is ever built.
 */
char *runtime_entry_expand(const char *template_text, const runtime_entry_values *values);

/** Whether every {{...}} in template_text names a placeholder of the closed set. */
int runtime_entry_template_is_known(const char *template_text);

#endif
