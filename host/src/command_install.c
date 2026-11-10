#include <stdlib.h>
#include <string.h>

#include "application_choices.h"
#include "boot.h"
#include "cache_refusal.h"
#include "host_commands.h"
#include "host_context.h"
#include "host_options.h"
#include "host_output.h"
#include "install_by_name.h"
#include "install_ecosystem.h"
#include "install_warning_report.h"
#include "terminal_question.h"

#define CHOICE_REFUSAL_PREFIX "the choice"
#define UNAVAILABLE_FIX "see each item's fix under unavailable above, then install again"

static int is_url_scheme_character(char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '+' || character == '.' || character == '-';
}

/** Whether catalog starts as <scheme>://, any scheme, as a url the user meant rather than a path. */
static int names_a_url(const char *catalog) {
    size_t length = 0;
    while (is_url_scheme_character(catalog[length])) {
        length++;
    }
    return length > 0 && ((catalog[0] >= 'a' && catalog[0] <= 'z') || (catalog[0] >= 'A' && catalog[0] <= 'Z')) &&
           strncmp(catalog + length, "://", 3) == 0;
}

/** The sources --catalog names: one https source, or one local source at its absolute path; NULL when misused. */
static cJSON *sources_of(const char *catalog, int *misused) {
    *misused = 0;
    cJSON *source = cJSON_CreateObject();
    if (source == NULL) {
        return NULL;
    }
    if (strncmp(catalog, "https://", 8) == 0) {
        cJSON_AddStringToObject(source, "kind", "https");
        cJSON_AddStringToObject(source, "url", catalog);
    } else if (names_a_url(catalog)) {
        *misused = 1;
        cJSON_Delete(source);
        return NULL;
    } else {
        char *absolute = host_absolute_path(catalog);
        cJSON_AddStringToObject(source, "kind", "local");
        cJSON_AddStringToObject(source, "path", absolute == NULL ? catalog : absolute);
        free(absolute);
    }
    cJSON *sources = cJSON_CreateArray();
    if (sources == NULL || !cJSON_AddItemToArray(sources, source)) {
        cJSON_Delete(source);
        cJSON_Delete(sources);
        return NULL;
    }
    return sources;
}

/** The --application id=method flags as {id: method}; NULL with *problem set when one is not that shape or repeats. */
static cJSON *choices_of(const cJSON *flags, char **problem) {
    cJSON *choices = cJSON_CreateObject();
    for (const cJSON *flag = flags == NULL || choices == NULL ? NULL : flags->child; flag != NULL; flag = flag->next) {
        const char *value = flag->valuestring;
        const char *equals = strchr(value, '=');
        size_t id_length = equals == NULL ? 0 : (size_t)(equals - value);
        char *id = id_length == 0 || equals[1] == 0 ? NULL : malloc(id_length + 1);
        if (id == NULL) {
            const char *parts[3] = {"--application ", value, " is not <id>=<method>"};
            *problem = runtime_cache_text(parts, 3);
            cJSON_Delete(choices);
            return NULL;
        }
        memcpy(id, value, id_length);
        id[id_length] = 0;
        int repeated = cJSON_HasObjectItem(choices, id);
        if (repeated || cJSON_AddStringToObject(choices, id, equals + 1) == NULL) {
            const char *parts[3] = {"--application names ", id, " twice"};
            *problem = repeated ? runtime_cache_text(parts, 3) : NULL;
            free(id);
            cJSON_Delete(choices);
            return NULL;
        }
        free(id);
    }
    return choices;
}

static cJSON *ecosystem_call(const host_context *context, const cJSON *sources, const cJSON *plugins, const cJSON *choices) {
    cJSON *call = host_places_argument(context);
    int built = call != NULL && cJSON_AddItemToObject(call, "sources", cJSON_Duplicate(sources, 1)) &&
                cJSON_AddItemToObject(call, "environment", cJSON_Duplicate(context->environment, 1)) &&
                cJSON_AddItemToObject(call, "choices", cJSON_Duplicate(choices, 1)) &&
                cJSON_AddItemToObject(call, "requested", plugins == NULL ? cJSON_CreateArray() : cJSON_Duplicate(plugins, 1));
    if (!built) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

static char *ask_on_the_terminal(const char *question, void *context) {
    (void)context;
    host_terminal terminal = host_controlling_terminal();
    return host_ask_on_terminal(question, &terminal);
}

/** Merges the methods asked for into choices; whether any was chosen. */
static int merge_asked(cJSON *choices, const cJSON *report) {
    cJSON *asked = host_choose_methods(cJSON_GetObjectItemCaseSensitive(report, "missing"), ask_on_the_terminal, NULL);
    int chosen = 0;
    for (cJSON *choice = asked == NULL ? NULL : asked->child; choice != NULL; choice = choice->next) {
        chosen = cJSON_AddStringToObject(choices, choice->string, choice->valuestring) != NULL || chosen;
    }
    cJSON_Delete(asked);
    return chosen;
}

/** boot.place for the context's cache: NULL when forebay-boot is in place, else its refusal. */
static cJSON *place_boot_stub(const host_context *context) {
    cJSON *argument = cJSON_CreateObject();
    if (argument != NULL && cJSON_AddStringToObject(argument, "cache", context->cache) == NULL) {
        cJSON_Delete(argument);
        argument = NULL;
    }
    cJSON *refusal = NULL;
    cJSON *placed = host_call(runtime_boot_place, argument, &refusal);
    cJSON_Delete(placed);
    if (placed == NULL && refusal == NULL) {
        const char *parts[1] = {"forebay-host ran out of memory"};
        refusal = host_refusal_of(parts, 1, "close other programs, then run install again");
    }
    return placed == NULL ? refusal : NULL;
}

/**
 * Installs once with the flags' choices, then, when the report leaves applications missing and the
 * terminal answers, once more with what was chosen there.
 *
 * @implNote A refusal of a choice is the command line's misuse, a flag naming an application or method
 * install does not offer, so it exits 2 with the usage as any misused flag does.
 */
static int install_through(const host_context *context, const cJSON *sources, const cJSON *plugins, cJSON *choices) {
    cJSON *refusal = NULL;
    cJSON *report = host_call(runtime_install_ecosystem, ecosystem_call(context, sources, plugins, choices), &refusal);
    if (report != NULL && merge_asked(choices, report)) {
        cJSON_Delete(report);
        report = host_call(runtime_install_ecosystem, ecosystem_call(context, sources, plugins, choices), &refusal);
    }
    if (report == NULL) {
        const char *message = host_text(refusal, "message");
        if (message != NULL && strncmp(message, CHOICE_REFUSAL_PREFIX, strlen(CHOICE_REFUSAL_PREFIX)) == 0) {
            int code = host_misuse(message);
            cJSON_Delete(refusal);
            return code;
        }
        return host_refuse(refusal);
    }
    host_print_json(report);
    host_print_install_warnings(context, report);
    cJSON *placed = place_boot_stub(context);
    const cJSON *unavailable = cJSON_GetObjectItemCaseSensitive(report, "unavailable");
    int code = placed != NULL ? host_refuse(placed) : HOST_EXIT_DONE;
    if (code == HOST_EXIT_DONE && cJSON_GetArraySize(unavailable) > 0) {
        char ids[2048] = "";
        for (const cJSON *row = unavailable->child; row != NULL; row = row->next) {
            const char *id = host_text(row, "id");
            if (id != NULL && strlen(ids) + strlen(id) + 3 < sizeof ids) {
                strcat(ids, ids[0] == 0 ? "" : ", ");
                strcat(ids, id);
            }
        }
        const char *parts[2] = {ids, " could not be installed"};
        code = host_refuse(host_refusal_of(parts, 2, UNAVAILABLE_FIX));
    }
    cJSON_Delete(report);
    return code;
}

#define OPTION_CATALOG 0
#define OPTION_PLUGIN 1
#define OPTION_APPLICATION 2
#define OPTION_NAME 3
#define OPTION_ACCEPT 4
#define OPTION_ACCEPT_PRIMARY 5
#define OPTION_COUNT 6

static int installs_from_catalog(const host_option *options) {
    const char *catalog = host_last_value(&options[OPTION_CATALOG]);
    char *problem = NULL;
    int misused = 0;
    cJSON *sources = catalog == NULL ? NULL : sources_of(catalog, &misused);
    cJSON *choices = sources == NULL ? NULL : choices_of(options[OPTION_APPLICATION].values, &problem);
    int code = HOST_EXIT_DONE;
    if (catalog == NULL) {
        code = host_misuse("--catalog <path or url> is required");
    } else if (misused) {
        const char *parts[3] = {"--catalog ", catalog, " is neither an https url nor a local path"};
        problem = runtime_cache_text(parts, 3);
        code = host_misuse(problem == NULL ? catalog : problem);
    } else if (choices == NULL) {
        code = host_misuse(problem == NULL ? "forebay-host ran out of memory" : problem);
    } else {
        host_context context;
        cJSON *refusal = NULL;
        code = host_open_context(&context, &refusal)
                   ? install_through(&context, sources, options[OPTION_PLUGIN].values, choices)
                   : host_refuse(refusal);
        host_close_context(&context);
    }
    free(problem);
    cJSON_Delete(choices);
    cJSON_Delete(sources);
    return code;
}

static int is_given(const host_option *option) {
    return host_last_value(option) != NULL;
}

static int is_by_name(const host_option *options) {
    return is_given(&options[OPTION_NAME]) || is_given(&options[OPTION_ACCEPT]) || is_given(&options[OPTION_ACCEPT_PRIMARY]);
}

/** The misuse --name's other flags are in, or NULL when there is none. */
static const char *by_name_misuse(const host_option *options) {
    if (!is_given(&options[OPTION_NAME])) {
        return "--accept and --accept-primary are only meaningful with --name <[marketplace/]name>";
    }
    if (is_given(&options[OPTION_CATALOG]) || is_given(&options[OPTION_PLUGIN])) {
        return "--name installs from the marketplaces in the settings and takes no --catalog or --plugin";
    }
    if (!is_given(&options[OPTION_APPLICATION])) {
        return "--name needs --application <id>, the registered application whose plugin folder the plugin is deployed into";
    }
    return NULL;
}

static int installs_by_name(const host_option *options) {
    const char *misuse = by_name_misuse(options);
    if (misuse != NULL) {
        return host_misuse(misuse);
    }
    host_name_request request = {host_last_value(&options[OPTION_NAME]), host_last_value(&options[OPTION_APPLICATION]),
                                 host_last_value(&options[OPTION_ACCEPT]), host_last_value(&options[OPTION_ACCEPT_PRIMARY])};
    host_context context;
    cJSON *refusal = NULL;
    int code = host_open_context(&context, &refusal) ? host_install_by_name(&context, &request) : host_refuse(refusal);
    host_close_context(&context);
    return code;
}

int host_command_install(int count, char **arguments) {
    host_option options[OPTION_COUNT] = {{"catalog", NULL}, {"plugin", NULL}, {"application", NULL}, {"name", NULL}, {"accept", NULL}, {"accept-primary", NULL}};
    char *problem = NULL;
    int code = HOST_EXIT_DONE;
    if (!host_parse_options(count, arguments, options, OPTION_COUNT, NULL, &problem)) {
        code = host_misuse(problem == NULL ? "forebay-host ran out of memory" : problem);
    } else if (is_by_name(options)) {
        code = installs_by_name(options);
    } else {
        code = installs_from_catalog(options);
    }
    free(problem);
    host_free_options(options, OPTION_COUNT);
    return code;
}
