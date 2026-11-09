#include <stdio.h>
#include <string.h>

#include "entry_template.h"
#include "loader_issues.h"

#define PATH_LIMIT 128
#define ROOTS_FIX "start it at {{home}} or {{entryRoot}}, with / between segments and no .. segment, so removal can reverse it"
#define KNOWN_FIX "use only home, entryRoot, loaderId, boot, bootedHost, hint and bundles, each as {{name}} or {{json:name}}"

static const char *const ROOTS[] = {"{{home}}", "{{entryRoot}}"};

static const cJSON *member(const cJSON *object, const char *name) {
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

static int is_text(const cJSON *value) {
    return cJSON_IsString(value) && value->valuestring[0] != '\0';
}

static void require_text(issue_list *list, const cJSON *owner, const char *name, const char *owner_path) {
    if (is_text(member(owner, name))) {
        return;
    }
    char path[PATH_LIMIT];
    char detail[PATH_LIMIT];
    snprintf(path, sizeof path, "%s/%s", owner_path, name);
    snprintf(detail, sizeof detail, "%s is not a non-empty string", name);
    runtime_loader_add_issue(list, path, detail, "declare it as a non-empty string");
}

static void check_state_file(issue_list *list, const cJSON *state_file) {
    if (state_file == NULL) {
        return;
    }
    const char *path = "/application/proxy/ownLogin/stateFile";
    if (!cJSON_IsObject(state_file)) {
        runtime_loader_add_issue(list, path, "stateFile is not an object", "declare {name, member} and optionally directoryVariable, or leave it out");
        return;
    }
    require_text(list, state_file, "name", path);
    require_text(list, state_file, "member", path);
    if (member(state_file, "directoryVariable") != NULL) {
        require_text(list, state_file, "directoryVariable", path);
    }
}

static int is_text_list(const cJSON *value) {
    if (!cJSON_IsArray(value)) {
        return 0;
    }
    const cJSON *item;
    cJSON_ArrayForEach(item, value) {
        if (!is_text(item)) {
            return 0;
        }
    }
    return 1;
}

static void check_own_login(issue_list *list, const cJSON *own_login) {
    if (!cJSON_IsObject(own_login)) {
        runtime_loader_add_issue(list, "/application/proxy/ownLogin", "ownLogin is not an object", "declare {variables} and optionally stateFile");
        return;
    }
    if (!is_text_list(member(own_login, "variables"))) {
        runtime_loader_add_issue(list, "/application/proxy/ownLogin/variables", "variables is not a list of variable names", "list the variables that carry a login of the application's own, or an empty list");
    }
    check_state_file(list, member(own_login, "stateFile"));
}

static void check_proxy(issue_list *list, const cJSON *proxy) {
    if (proxy == NULL) {
        return;
    }
    if (!cJSON_IsObject(proxy)) {
        runtime_loader_add_issue(list, "/application/proxy", "proxy is not an object", "declare baseUrlVariable, placeholder and ownLogin, or leave proxy out");
        return;
    }
    require_text(list, proxy, "baseUrlVariable", "/application/proxy");
    const cJSON *placeholder = member(proxy, "placeholder");
    if (cJSON_IsObject(placeholder)) {
        require_text(list, placeholder, "variable", "/application/proxy/placeholder");
        require_text(list, placeholder, "value", "/application/proxy/placeholder");
    } else {
        runtime_loader_add_issue(list, "/application/proxy/placeholder", "placeholder is not an object", "declare {variable, value}");
    }
    check_own_login(list, member(proxy, "ownLogin"));
}

static int starts_at_a_root(const char *text) {
    for (size_t index = 0; index < sizeof ROOTS / sizeof ROOTS[0]; index++) {
        size_t length = strlen(ROOTS[index]);
        if (strncmp(text, ROOTS[index], length) == 0 && (text[length] == '\0' || text[length] == '/')) {
            return 1;
        }
    }
    return 0;
}

static int has_a_climbing_segment(const char *text) {
    for (const char *segment = text; segment != NULL; segment = strchr(segment, '/')) {
        segment += *segment == '/';
        size_t length = strcspn(segment, "/");
        if (length == 2 && strncmp(segment, "..", 2) == 0) {
            return 1;
        }
    }
    return 0;
}

static int is_reversible_path(const cJSON *value) {
    return is_text(value) && strchr(value->valuestring, '\\') == NULL && starts_at_a_root(value->valuestring) &&
           !has_a_climbing_segment(value->valuestring) && runtime_entry_template_is_known(value->valuestring);
}

static void require_path(issue_list *list, const cJSON *value, const char *path) {
    if (!is_reversible_path(value)) {
        runtime_loader_add_issue(list, path, "this path is not one removal can reverse", ROOTS_FIX);
    }
}

/** Whether every string inside value is a known template; a member's name is written as it stands. */
static int templates_are_known(const cJSON *value) {
    if (cJSON_IsString(value)) {
        return runtime_entry_template_is_known(value->valuestring);
    }
    const cJSON *child;
    cJSON_ArrayForEach(child, value) {
        if (!templates_are_known(child)) {
            return 0;
        }
    }
    return 1;
}

static void require_known(issue_list *list, const cJSON *value, const char *path) {
    if (!templates_are_known(value)) {
        runtime_loader_add_issue(list, path, "this names a placeholder that does not exist", KNOWN_FIX);
    }
}

static void check_file_content(issue_list *list, const cJSON *file, const char *file_path) {
    const cJSON *json = member(file, "json");
    const cJSON *text = member(file, "text");
    char path[PATH_LIMIT];
    if ((json == NULL) == (text == NULL)) {
        runtime_loader_add_issue(list, file_path, "a file declares exactly one of json and text", "give the file a json value or a text, not both");
    } else if (json != NULL) {
        snprintf(path, sizeof path, "%s/json", file_path);
        require_known(list, json, path);
    } else if (!cJSON_IsString(text)) {
        snprintf(path, sizeof path, "%s/text", file_path);
        runtime_loader_add_issue(list, path, "text is not a string", "give the file's content as a string");
    } else {
        snprintf(path, sizeof path, "%s/text", file_path);
        require_known(list, text, path);
    }
}

static void check_file(issue_list *list, const cJSON *file, int index) {
    char file_path[PATH_LIMIT];
    char path[PATH_LIMIT + 8];
    snprintf(file_path, sizeof file_path, "/application/entry/files/%d", index);
    snprintf(path, sizeof path, "%s/path", file_path);
    require_path(list, member(file, "path"), path);
    check_file_content(list, file, file_path);
}

static void check_setting(issue_list *list, const cJSON *setting, int index) {
    char setting_path[PATH_LIMIT];
    char path[PATH_LIMIT + 8];
    snprintf(setting_path, sizeof setting_path, "/application/entry/settings/%d", index);
    snprintf(path, sizeof path, "%s/file", setting_path);
    require_path(list, member(setting, "file"), path);
    snprintf(path, sizeof path, "%s/key", setting_path);
    const cJSON *key = member(setting, "key");
    if (!is_text_list(key) || key->child == NULL) {
        runtime_loader_add_issue(list, path, "key is not a list of member names", "name the path of the settings member as a non-empty list of strings");
    } else {
        require_known(list, key, path);
    }
    snprintf(path, sizeof path, "%s/value", setting_path);
    const cJSON *value = member(setting, "value");
    if (value == NULL) {
        runtime_loader_add_issue(list, path, "the setting has no value", "give the setting the value it writes");
    } else {
        require_known(list, value, path);
    }
}

typedef void (*item_check)(issue_list *list, const cJSON *item, int index);

static void check_list(issue_list *list, const cJSON *items, const char *name, item_check check) {
    char path[PATH_LIMIT];
    snprintf(path, sizeof path, "/application/entry/%s", name);
    if (!cJSON_IsArray(items)) {
        runtime_loader_add_issue(list, path, "this is not a list", "declare it as a list, empty when there is nothing to write");
        return;
    }
    int index = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, items) {
        check(list, item, index++);
    }
}

static void check_directory(issue_list *list, const cJSON *directory, int index) {
    char path[PATH_LIMIT];
    snprintf(path, sizeof path, "/application/entry/directories/%d", index);
    require_path(list, directory, path);
}

static void check_entry(issue_list *list, const cJSON *entry) {
    if (entry == NULL) {
        return;
    }
    if (!cJSON_IsObject(entry)) {
        runtime_loader_add_issue(list, "/application/entry", "entry is not an object", "declare runsOnJavascriptHost, directories, files and settings, or leave entry out");
        return;
    }
    if (!cJSON_IsBool(member(entry, "runsOnJavascriptHost"))) {
        runtime_loader_add_issue(list, "/application/entry/runsOnJavascriptHost", "runsOnJavascriptHost is not a boolean", "say whether the entry runs the loader's host on the JavaScript host bundle");
    }
    check_list(list, member(entry, "directories"), "directories", check_directory);
    check_list(list, member(entry, "files"), "files", check_file);
    check_list(list, member(entry, "settings"), "settings", check_setting);
}

void runtime_loader_check_application_facts(issue_list *list, const cJSON *application) {
    check_proxy(list, member(application, "proxy"));
    check_entry(list, member(application, "entry"));
}
