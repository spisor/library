#include "ui_refusal.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "json_service.h"

static size_t joined_length(va_list parts) {
    size_t length = 0;
    for (const char *part = va_arg(parts, const char *); part != NULL; part = va_arg(parts, const char *)) {
        length += strlen(part);
    }
    return length;
}

static void join(char *message, va_list parts) {
    message[0] = '\0';
    for (const char *part = va_arg(parts, const char *); part != NULL; part = va_arg(parts, const char *)) {
        strcat(message, part);
    }
}

cJSON *runtime_ui_refusal(const char *fix, ...) {
    va_list parts;
    va_start(parts, fix);
    size_t length = joined_length(parts);
    va_end(parts);
    char *message = malloc(length + 1);
    if (message == NULL) {
        return NULL;
    }
    va_start(parts, fix);
    join(message, parts);
    va_end(parts);
    cJSON *refusal = runtime_refusal(message, fix);
    free(message);
    return refusal;
}
