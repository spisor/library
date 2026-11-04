#ifndef RUNTIME_CATALOG_PATTERNS_H
#define RUNTIME_CATALOG_PATTERNS_H

/** Each answers what the schema's pattern of the same name answers as a JavaScript regular expression. */
int runtime_catalog_is_item_id(const char *key);

int runtime_catalog_is_version(const char *key);

int runtime_catalog_is_file_key(const char *key);

int runtime_catalog_is_sha256(const char *text);

int runtime_catalog_is_name(const char *text);

int runtime_catalog_is_candidate(const char *text);

#endif
