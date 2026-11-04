#ifndef RUNTIME_CATALOG_FOLD_TABLE_H
#define RUNTIME_CATALOG_FOLD_TABLE_H

#include <stddef.h>
#include <stdint.h>

/** One code point and the run of runtime_catalog_fold_pool it folds to. */
typedef struct runtime_catalog_fold_entry {
    uint32_t code_point;
    uint16_t start;
    uint8_t length;
} runtime_catalog_fold_entry;

/** A run of code points sharing one nonzero canonical combining class. */
typedef struct runtime_catalog_class_range {
    uint32_t first;
    uint32_t last;
    uint8_t combining_class;
} runtime_catalog_class_range;

/** Sorted by code_point. */
extern const runtime_catalog_fold_entry runtime_catalog_fold_entries[];
extern const size_t runtime_catalog_fold_entry_count;
extern const uint32_t runtime_catalog_fold_pool[];

/** Sorted by first. */
extern const runtime_catalog_class_range runtime_catalog_class_ranges[];
extern const size_t runtime_catalog_class_range_count;

#endif
