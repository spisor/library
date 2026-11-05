#ifndef RUNTIME_CATALOG_FOLD_H
#define RUNTIME_CATALOG_FOLD_H

#include <stddef.h>

/**
 * Whether two of keys would name one file on a file system that ignores case and normalization, as
 * Windows and macOS do by default; when so, *first and *second are the indexes of the earliest such pair,
 * *first being the earlier key that the later one repeats. Answers -1 when memory runs out.
 *
 * @implNote Compares the canonical decomposition of each code point's lowercase decomposition, in canonical order, from a table
 * scripts/catalog-fold-table.py generates; it answers alike for every pair runtime/test/fixtures/catalog/folds.json
 * names. Lowercasing is per code point, so a final capital sigma folds to sigma where JavaScript gives a
 * final sigma.
 */
int runtime_catalog_folded_duplicate(const char *const *keys, size_t count, size_t *first, size_t *second);

#endif
