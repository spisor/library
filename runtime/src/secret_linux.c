#include "secret_backend.h"

int runtime_secret_store_available(void) {
    return 0;
}

runtime_secret_answer runtime_secret_store_get(const char *entry, char **value) {
    (void)entry;
    *value = NULL;
    return RUNTIME_SECRET_UNAVAILABLE;
}

runtime_secret_answer runtime_secret_store_set(const char *entry, const char *value) {
    (void)entry;
    (void)value;
    return RUNTIME_SECRET_UNAVAILABLE;
}

runtime_secret_answer runtime_secret_store_delete(const char *entry) {
    (void)entry;
    return RUNTIME_SECRET_UNAVAILABLE;
}

runtime_secret_answer runtime_secret_store_list(const char *prefix, runtime_secret_names *names) {
    (void)prefix;
    names->names = NULL;
    names->count = 0;
    return RUNTIME_SECRET_UNAVAILABLE;
}
