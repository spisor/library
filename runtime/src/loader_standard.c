#include "loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json_service.h"
#include "spisor/json_kind.h"

#define PORTABLE_NAME_LIMIT 64
#define PORTABLE_NAME_FIX                                                                                     \
    "name it as one portable file name: a letter or digit, then letters, digits, \".\", \"_\" or \"-\", at " \
    "most 64 characters"
#define PATH_COMMANDS_FIX "declare the PATH commands as {\"requested\": [\"name\"], \"minimum\": 0 or 1}"

/** The issues found so far; complete drops to 0 when memory runs out, and every later add is skipped. */
typedef struct issue_list {
    cJSON *issues;
    int complete;
} issue_list;

static void add_issue(issue_list *list, const char *path, const char *detail, const char *fix) {
    cJSON *issue = list->complete ? cJSON_CreateObject() : NULL;
    int added = issue != NULL && cJSON_AddStringToObject(issue, "path", path) != NULL &&
                cJSON_AddStringToObject(issue, "detail", detail) != NULL &&
                cJSON_AddStringToObject(issue, "fix", fix) != NULL && cJSON_AddItemToArray(list->issues, issue);
    if (!added) {
        cJSON_Delete(issue);
        list->complete = 0;
    }
}

/** What JSON.stringify writes for value, undefined for none, freed with free; NULL when memory runs out. */
static char *written(const cJSON *value) {
    char *text = value == NULL ? NULL : cJSON_PrintUnformatted(value);
    if (value == NULL) {
        text = malloc(sizeof "undefined");
        if (text != NULL) {
            memcpy(text, "undefined", sizeof "undefined");
        }
    }
    return text;
}

/** Adds an issue whose detail is before, value as JSON.stringify writes it, then after. */
static void add_issue_naming(issue_list *list, const char *path, const char *before, const cJSON *value,
                             const char *after, const char *fix) {
    char *text = list->complete ? written(value) : NULL;
    size_t size = text == NULL ? 0 : strlen(before) + strlen(text) + strlen(after) + 1;
    char *detail = size == 0 ? NULL : malloc(size);
    if (detail == NULL) {
        list->complete = 0;
    } else {
        snprintf(detail, size, "%s%s%s", before, text, after);
        add_issue(list, path, detail, fix);
    }
    free(detail);
    cJSON_free(text);
}

static int is_record(const cJSON *value) {
    return cJSON_IsObject(value);
}

static int is_non_empty_string(const cJSON *value) {
    return cJSON_IsString(value) && value->valuestring[0] != '\0';
}

static int is_letter_or_digit(char letter) {
    return (letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z') || (letter >= '0' && letter <= '9');
}

static int is_portable_text(const char *text) {
    size_t length = strlen(text);
    if (length == 0 || length > PORTABLE_NAME_LIMIT || !is_letter_or_digit(text[0])) {
        return 0;
    }
    for (size_t index = 1; index < length; index++) {
        if (!is_letter_or_digit(text[index]) && strchr("._-", text[index]) == NULL) {
            return 0;
        }
    }
    return 1;
}

static int is_portable_name(const cJSON *value) {
    return cJSON_IsString(value) && is_portable_text(value->valuestring);
}

/** @implNote "C:/" or "/" at the start counts as absolute, the TypeScript check's own rule; a backslash does not. */
static int is_home_candidate(const char *candidate) {
    int drive = ((candidate[0] >= 'A' && candidate[0] <= 'Z') || (candidate[0] >= 'a' && candidate[0] <= 'z')) &&
                candidate[1] == ':' && candidate[2] == '/';
    return strncmp(candidate, "~/", 2) == 0 || candidate[0] == '/' || drive;
}

static const cJSON *member(const cJSON *object, const char *name) {
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

static void check_text(issue_list *list, const cJSON *application, const char *name) {
    if (is_non_empty_string(member(application, name))) {
        return;
    }
    char path[64];
    char detail[64];
    char fix[96];
    snprintf(path, sizeof path, "/application/%s", name);
    snprintf(detail, sizeof detail, "the application has no %s", name);
    snprintf(fix, sizeof fix, "name the application's %s as a non-empty string", name);
    add_issue(list, path, detail, fix);
}

static void check_binary(issue_list *list, const cJSON *binary) {
    if (!is_portable_name(binary)) {
        add_issue_naming(list, "/application/binary", "the binary ", binary, " is not a portable file name",
                         PORTABLE_NAME_FIX "; a path is not a binary name");
    }
}

static void check_optional_name(issue_list *list, const cJSON *home, const char *name) {
    const cJSON *value = member(home, name);
    if (value == NULL || is_non_empty_string(value)) {
        return;
    }
    char path[64];
    char detail[96];
    char fix[128];
    snprintf(path, sizeof path, "/application/home/%s", name);
    snprintf(detail, sizeof detail, "%s is not an environment variable name", name);
    snprintf(fix, sizeof fix, "name the variable as a non-empty string, or leave %s out", name);
    add_issue(list, path, detail, fix);
}

static void check_xdg_subdir(issue_list *list, const cJSON *xdg_subdir) {
    if (xdg_subdir != NULL && !is_portable_name(xdg_subdir)) {
        add_issue_naming(list, "/application/home/xdgSubdir", "the xdgSubdir ", xdg_subdir,
                         " is not a portable directory name", PORTABLE_NAME_FIX ", or leave xdgSubdir out");
    }
}

static void check_candidates(issue_list *list, const cJSON *candidates) {
    if (!cJSON_IsArray(candidates) || cJSON_GetArraySize(candidates) == 0) {
        add_issue(list, "/application/home/candidates", "the home has no candidate directories",
                  "list at least one candidate, for example \"~/.claude\"");
        return;
    }
    int index = 0;
    const cJSON *candidate;
    cJSON_ArrayForEach(candidate, candidates) {
        if (!cJSON_IsString(candidate) || !is_home_candidate(candidate->valuestring)) {
            char path[64];
            snprintf(path, sizeof path, "/application/home/candidates/%d", index);
            add_issue_naming(list, path, "the candidate ", candidate, " is neither rooted at ~/ nor absolute",
                             "root the candidate at \"~/\" or write an absolute path");
        }
        index++;
    }
}

static void check_home(issue_list *list, const cJSON *home) {
    if (!is_record(home)) {
        add_issue(list, "/application/home", "the application has no home",
                  "give the application a home with a candidates list");
        return;
    }
    check_optional_name(list, home, "envOverride");
    check_optional_name(list, home, "nativeEnv");
    check_xdg_subdir(list, member(home, "xdgSubdir"));
    check_candidates(list, member(home, "candidates"));
}

static int is_package_mapping(const cJSON *value) {
    if (cJSON_IsString(value)) {
        return 1;
    }
    const cJSON *options = member(value, "options");
    return is_record(value) && cJSON_IsString(member(value, "name")) && (options == NULL || is_record(options));
}

static void check_package(issue_list *list, const cJSON *mapping) {
    const char *method = mapping->string;
    size_t size = strlen(method) + 64;
    char *path = malloc(size);
    char *detail = malloc(strlen(method) + 128);
    if (path == NULL || detail == NULL) {
        list->complete = 0;
    } else {
        snprintf(path, size, "/application/packages/%s", method);
        cJSON *name = cJSON_CreateString(method);
        if (name == NULL) {
            list->complete = 0;
        } else if (!is_portable_text(method)) {
            add_issue_naming(list, path, "", name, " is not an install method id", PORTABLE_NAME_FIX ", for example \"npm\"");
        }
        cJSON_Delete(name);
        if (!is_package_mapping(mapping)) {
            snprintf(detail, strlen(method) + 128,
                     "the package under %s is neither a name nor an object with a name and optional options object",
                     method);
            add_issue(list, path, detail, "write the package as \"name\" or {\"name\": \"name\", \"options\": {}}");
        }
    }
    free(path);
    free(detail);
}

static void check_packages(issue_list *list, const cJSON *packages) {
    if (!is_record(packages)) {
        add_issue(list, "/application/packages", "the application has no packages",
                  "map each install method id to a package name");
        return;
    }
    const cJSON *mapping;
    cJSON_ArrayForEach(mapping, packages) {
        check_package(list, mapping);
    }
}

static void check_application(issue_list *list, const cJSON *application) {
    if (!is_record(application)) {
        add_issue(list, "/application", "the standard has no application",
                  "describe the application with an id, label, binary, home and packages");
        return;
    }
    check_text(list, application, "id");
    check_text(list, application, "label");
    check_binary(list, member(application, "binary"));
    check_home(list, member(application, "home"));
    check_packages(list, member(application, "packages"));
}

static int appears_before(const cJSON *requested, const cJSON *name) {
    for (const cJSON *earlier = requested->child; earlier != name; earlier = earlier->next) {
        if (strcmp(earlier->valuestring, name->valuestring) == 0) {
            return 1;
        }
    }
    return 0;
}

static int is_unique_portable_list(const cJSON *requested) {
    if (!cJSON_IsArray(requested)) {
        return 0;
    }
    const cJSON *name;
    cJSON_ArrayForEach(name, requested) {
        if (!is_portable_name(name) || appears_before(requested, name)) {
            return 0;
        }
    }
    return 1;
}

static void check_path_commands(issue_list *list, const cJSON *path_commands) {
    if (!is_record(path_commands)) {
        add_issue(list, "/pathCommands", "the standard has no pathCommands", PATH_COMMANDS_FIX);
        return;
    }
    const cJSON *minimum = member(path_commands, "minimum");
    if (!is_unique_portable_list(member(path_commands, "requested"))) {
        add_issue(list, "/pathCommands/requested", "requested is not a list of unique portable command names",
                  PATH_COMMANDS_FIX "; " PORTABLE_NAME_FIX);
    }
    if (!cJSON_IsNumber(minimum) || (minimum->valuedouble != 0 && minimum->valuedouble != 1)) {
        add_issue(list, "/pathCommands/minimum", "minimum is neither 0 nor 1",
                  "set \"minimum\" to 0 when the host works without a command on PATH, or 1 when it needs one");
    }
}

static void check_background(issue_list *list, const cJSON *background) {
    int valid = cJSON_IsArray(background);
    const cJSON *ids = valid ? background : NULL;
    const cJSON *id;
    cJSON_ArrayForEach(id, ids) {
        valid = valid && is_non_empty_string(id);
    }
    if (!valid) {
        add_issue(list, "/background", "background is not a list of host ids",
                  "list the ids of the hosts to keep running, or an empty list");
    }
}

cJSON *runtime_loader_standard_issues(const cJSON *value) {
    issue_list list = {cJSON_CreateArray(), 1};
    list.complete = list.issues != NULL;
    if (!is_record(value)) {
        add_issue(&list, "", "the standard is not an object", "give the standard an application, pathCommands and background");
    } else {
        check_application(&list, member(value, "application"));
        check_path_commands(&list, member(value, "pathCommands"));
        check_background(&list, member(value, "background"));
    }
    if (!list.complete) {
        cJSON_Delete(list.issues);
        return NULL;
    }
    return list.issues;
}

static cJSON *check_for(const cJSON *argument, cJSON **refusal) {
    (void)refusal;
    cJSON *answer = cJSON_CreateObject();
    cJSON *issues = runtime_loader_standard_issues(cJSON_GetObjectItemCaseSensitive(argument, "standard"));
    if (answer == NULL || issues == NULL || !cJSON_AddItemToObject(answer, "issues", issues)) {
        cJSON_Delete(answer);
        cJSON_Delete(issues);
        return NULL;
    }
    return answer;
}

RUNTIME_EXPORT int32_t runtime_loader_check(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, check_for);
}
