#ifndef RUNTIME_IMPORT_NAME_H
#define RUNTIME_IMPORT_NAME_H

/**
 * Whether name satisfies the importName pattern of plugin.schema.json: words of lowercase letters and digits joined
 * by single hyphens or dots, not starting with a Windows reserved device name followed by a dot or the end.
 */
int runtime_is_import_name_grammar(const char *name);

/**
 * Whether name is usable as a plugin's import name: it satisfies the grammar and is not "package", whose deployed
 * file would replace the module marker package.json that deploy writes beside the plugins.
 */
int runtime_is_import_name(const char *name);

#endif
