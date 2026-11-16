#ifndef SPISOR_ENTRY_ORDER_H
#define SPISOR_ENTRY_ORDER_H

#include "spisor/value.h"

/**
 * A record's entries sorted by key, bytes first and length second, so a key held twice sits beside its twin. The
 * array is released with spisor_memory_release; a record of fewer than two entries answers NULL and allocates nothing.
 *
 * @return OK; OUT_OF_MEMORY with `*sorted` NULL.
 */
spisor_status spisor_entries_by_key(const spisor_value *record, const spisor_entry ***sorted);

int spisor_entries_share_a_key(const spisor_entry *left, const spisor_entry *right);

#endif
