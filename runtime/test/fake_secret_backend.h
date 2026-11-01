#ifndef RUNTIME_FAKE_SECRET_BACKEND_H
#define RUNTIME_FAKE_SECRET_BACKEND_H

#include "secret_backend.h"

typedef enum fake_store_behaviour {
    FAKE_STORE_ACCEPTS,
    FAKE_STORE_DECLINES,
    FAKE_STORE_UNAVAILABLE,
    FAKE_STORE_FAILS
} fake_store_behaviour;

/** Empties the store, makes it accept, and zeroes every counter. */
void fake_store_reset(void);

void fake_store_behave(fake_store_behaviour behaviour);

/** While accepting, the next successes sets succeed and every set after them fails. */
void fake_store_fail_sets_after(int successes);

/** Puts an entry in directly, as if an earlier process had stored it; counts no call. */
void fake_store_seed(const char *entry, const char *value);

/** The stored value, or NULL; counts no call. */
const char *fake_store_value(const char *entry);

int fake_store_entry_count(void);

int fake_store_calls(void);

int fake_store_set_calls(void);

#endif
