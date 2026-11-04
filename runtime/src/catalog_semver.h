#ifndef RUNTIME_CATALOG_SEMVER_H
#define RUNTIME_CATALOG_SEMVER_H

/**
 * Semver precedence as a sign: below zero when left comes first. A name that is not a version sorts below
 * every version, and among such names by text.
 */
int runtime_catalog_compare_versions(const char *left, const char *right);

#endif
