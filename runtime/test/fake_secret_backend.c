#include "fake_secret_backend.h"

#include <stdlib.h>
#include <string.h>

#define FAKE_STORE_CAPACITY 64

typedef struct fake_entry {
    char *name;
    char *value;
} fake_entry;

static fake_entry entries[FAKE_STORE_CAPACITY];
static int entry_count = 0;
static fake_store_behaviour current_behaviour = FAKE_STORE_ACCEPTS;
static int successes_left = -1;
static int calls = 0;
static int set_calls = 0;

static char *copy_text(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy != NULL) {
        memcpy(copy, text, size);
    }
    return copy;
}

static int index_of(const char *name) {
    for (int index = 0; index < entry_count; index++) {
        if (strcmp(entries[index].name, name) == 0) {
            return index;
        }
    }
    return -1;
}

static void remove_at(int index) {
    free(entries[index].name);
    free(entries[index].value);
    entries[index] = entries[--entry_count];
}

static int put(const char *name, const char *value) {
    char *copy = copy_text(value);
    int index = index_of(name);
    if (copy == NULL || (index < 0 && entry_count == FAKE_STORE_CAPACITY)) {
        free(copy);
        return 0;
    }
    if (index < 0) {
        index = entry_count++;
        entries[index].name = copy_text(name);
        entries[index].value = NULL;
    }
    free(entries[index].value);
    entries[index].value = copy;
    return entries[index].name != NULL;
}

static runtime_secret_answer refusal(void) {
    switch (current_behaviour) {
    case FAKE_STORE_DECLINES:
        return RUNTIME_SECRET_DECLINED;
    case FAKE_STORE_UNAVAILABLE:
        return RUNTIME_SECRET_UNAVAILABLE;
    case FAKE_STORE_FAILS:
        return RUNTIME_SECRET_FAILED;
    default:
        return RUNTIME_SECRET_DONE;
    }
}

void fake_store_reset(void) {
    while (entry_count > 0) {
        remove_at(0);
    }
    current_behaviour = FAKE_STORE_ACCEPTS;
    successes_left = -1;
    calls = 0;
    set_calls = 0;
}

void fake_store_behave(fake_store_behaviour behaviour) {
    current_behaviour = behaviour;
}

void fake_store_fail_sets_after(int successes) {
    successes_left = successes;
}

void fake_store_seed(const char *entry, const char *value) {
    put(entry, value);
}

const char *fake_store_value(const char *entry) {
    int index = index_of(entry);
    return index < 0 ? NULL : entries[index].value;
}

int fake_store_entry_count(void) {
    return entry_count;
}

int fake_store_calls(void) {
    return calls;
}

int fake_store_set_calls(void) {
    return set_calls;
}

int runtime_secret_store_available(void) {
    calls++;
    return current_behaviour != FAKE_STORE_UNAVAILABLE;
}

runtime_secret_answer runtime_secret_store_get(const char *entry, char **value) {
    calls++;
    *value = NULL;
    if (refusal() != RUNTIME_SECRET_DONE) {
        return refusal();
    }
    const char *stored = fake_store_value(entry);
    if (stored == NULL) {
        return RUNTIME_SECRET_ABSENT;
    }
    *value = copy_text(stored);
    return *value != NULL ? RUNTIME_SECRET_DONE : RUNTIME_SECRET_FAILED;
}

runtime_secret_answer runtime_secret_store_set(const char *entry, const char *value) {
    calls++;
    set_calls++;
    if (refusal() != RUNTIME_SECRET_DONE) {
        return refusal();
    }
    if (successes_left == 0) {
        return RUNTIME_SECRET_FAILED;
    }
    if (successes_left > 0) {
        successes_left--;
    }
    return put(entry, value) ? RUNTIME_SECRET_DONE : RUNTIME_SECRET_FAILED;
}

runtime_secret_answer runtime_secret_store_delete(const char *entry) {
    calls++;
    if (refusal() != RUNTIME_SECRET_DONE) {
        return refusal();
    }
    int index = index_of(entry);
    if (index < 0) {
        return RUNTIME_SECRET_ABSENT;
    }
    remove_at(index);
    return RUNTIME_SECRET_DONE;
}

runtime_secret_answer runtime_secret_store_list(const char *prefix, runtime_secret_names *names) {
    calls++;
    names->names = NULL;
    names->count = 0;
    if (refusal() != RUNTIME_SECRET_DONE) {
        return refusal();
    }
    names->names = calloc(FAKE_STORE_CAPACITY, sizeof(char *));
    if (names->names == NULL) {
        return RUNTIME_SECRET_FAILED;
    }
    size_t prefix_length = strlen(prefix);
    for (int index = 0; index < entry_count; index++) {
        if (strncmp(entries[index].name, prefix, prefix_length) == 0) {
            names->names[names->count++] = copy_text(entries[index].name);
        }
    }
    return RUNTIME_SECRET_DONE;
}
