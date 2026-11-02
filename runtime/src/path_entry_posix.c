#include "path_entry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "directory_tree.h"
#include "spisor/path.h"

static const char *const profile_names[] = {".profile", ".bashrc", ".zshrc"};

static const char *text_of(const cJSON *object, const char *member) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, member));
}

static runtime_change_step out_of_memory(runtime_change_note *note) {
    snprintf(note->detail, sizeof note->detail, "memory ran out");
    return RUNTIME_CHANGE_FAILED;
}

/**
 * @implNote The line quotes the directory in double quotes, inside which the shell still expands $,
 * ` and \, and a : would split it into two PATH entries, so those are refused rather than escaped.
 */
const char *runtime_path_entry_defect(const char *directory) {
    return strpbrk(directory, "\"\\$`:\r\n") == NULL
               ? NULL
               : "a path-entry directory cannot hold \", \\, $, `, : or a line break, which its profile line would "
                 "expand or split";
}

static char *export_line(const char *directory) {
    size_t size = strlen(directory) + sizeof "export PATH=\":$PATH\"";
    char *line = malloc(size);
    if (line != NULL) {
        snprintf(line, size, "export PATH=\"%s:$PATH\"", directory);
    }
    return line;
}

static int add_child(cJSON *children, const char *home, const char *name, const char *line) {
    const char *segments[2] = {home, name};
    char *file = spisor_path_join(0, segments, 2);
    if (file == NULL) {
        return 0;
    }
    int added = 1;
    if (runtime_probe_entry(file) != RUNTIME_ENTRY_MISSING) {
        cJSON *child = cJSON_CreateObject();
        added = child != NULL && cJSON_AddStringToObject(child, "kind", "profile-line") != NULL &&
                cJSON_AddStringToObject(child, "file", file) != NULL &&
                cJSON_AddStringToObject(child, "line", line) != NULL && cJSON_AddItemToArray(children, child);
        if (!added) {
            cJSON_Delete(child);
        }
    }
    free(file);
    return added;
}

static cJSON *existing_profile_lines(const cJSON *change) {
    cJSON *children = cJSON_CreateArray();
    char *line = export_line(text_of(change, "directory"));
    int complete = children != NULL && line != NULL;
    for (size_t index = 0; complete && index < sizeof profile_names / sizeof profile_names[0]; index++) {
        complete = add_child(children, text_of(change, "home"), profile_names[index], line);
    }
    free(line);
    if (!complete) {
        cJSON_Delete(children);
        return NULL;
    }
    return children;
}

runtime_change_step runtime_path_entry_children(const cJSON *change, cJSON **children, runtime_change_note *note) {
    *children = existing_profile_lines(change);
    if (*children == NULL) {
        return out_of_memory(note);
    }
    if (cJSON_GetArraySize(*children) == 0) {
        snprintf(note->detail, sizeof note->detail,
                 "none of .profile, .bashrc and .zshrc exists in %s, so the directory cannot be put on PATH; none "
                 "was created",
                 text_of(change, "home"));
        cJSON_Delete(*children);
        *children = NULL;
        return RUNTIME_CHANGE_REFUSED;
    }
    return RUNTIME_CHANGE_DONE;
}

runtime_change_step runtime_path_entry_prepare(const cJSON *change, const cJSON *holder, cJSON **record,
                                               runtime_change_note *note) {
    (void)holder;
    *record = cJSON_CreateObject();
    int complete = *record != NULL && cJSON_AddStringToObject(*record, "directory", text_of(change, "directory")) &&
                   cJSON_AddStringToObject(*record, "home", text_of(change, "home"));
    if (!complete) {
        cJSON_Delete(*record);
        *record = NULL;
        return out_of_memory(note);
    }
    return RUNTIME_CHANGE_DONE;
}

runtime_change_step runtime_path_entry_perform(const cJSON *change, cJSON *record, runtime_change_note *note) {
    (void)change;
    (void)record;
    (void)note;
    return RUNTIME_CHANGE_DONE;
}

int runtime_path_entry_is_on_disk(const cJSON *record) {
    cJSON *children = existing_profile_lines(record);
    int on_disk = 0;
    const cJSON *child;
    cJSON_ArrayForEach(child, children) {
        on_disk = on_disk || runtime_change_is_on_disk(child);
    }
    cJSON_Delete(children);
    return on_disk;
}

/**
 * @implNote The profile lines are the children, recorded after the path-entry and so reversed before
 * it, each under its own reference count; the path-entry itself has nothing left to undo.
 */
runtime_change_reversal runtime_path_entry_reverse(const cJSON *record, int held, runtime_change_note *note) {
    (void)record;
    (void)held;
    (void)note;
    return RUNTIME_CHANGE_REVERSED;
}
