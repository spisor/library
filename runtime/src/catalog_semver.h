#ifndef RUNTIME_CATALOG_SEMVER_H
#define RUNTIME_CATALOG_SEMVER_H

/**
 * Semver precedence as a sign: below zero when left comes first. A name that is not a version sorts below
 * every version, and among such names by text.
 */
int runtime_catalog_compare_versions(const char *left, const char *right);

/** Whether text is a version of three numbers and nothing else, no prerelease or build: 22.18.0, not 22.18 or 1.0.0-rc. */
int runtime_catalog_is_plain_version(const char *text);

#endif
