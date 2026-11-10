#include "import_name.h"

#include <string.h>

static int is_name_character(char character) {
    return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
}

static int is_reserved_device_stem(const char *name) {
    static const char *const reserved[] = {"con", "prn", "aux", "nul"};
    size_t stem_length = strcspn(name, ".");
    for (size_t index = 0; index < sizeof reserved / sizeof reserved[0]; index++) {
        if (stem_length == 3 && strncmp(name, reserved[index], 3) == 0) {
            return 1;
        }
    }
    int numbered = stem_length == 4 && (strncmp(name, "com", 3) == 0 || strncmp(name, "lpt", 3) == 0);
    return numbered && name[3] >= '1' && name[3] <= '9';
}

int runtime_is_import_name_grammar(const char *name) {
    if (!is_name_character(name[0]) || is_reserved_device_stem(name)) {
        return 0;
    }
    for (const char *at = name + 1; *at != '\0'; at++) {
        int separator = *at == '-' || *at == '.';
        if (!(is_name_character(*at) || (separator && is_name_character(at[1])))) {
            return 0;
        }
    }
    return 1;
}

int runtime_is_import_name(const char *name) {
    return runtime_is_import_name_grammar(name) && strcmp(name, "package") != 0;
}
