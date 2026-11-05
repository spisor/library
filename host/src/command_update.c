#include "catalog_time.h"
#include "file_times.h"
#include "host_commands.h"
#include "host_context.h"
#include "host_output.h"
#include "updates.h"

#define UPDATE_FIX "see each one's fix under refused above, then update again"

static cJSON *run_call(const host_context *context) {
    char now[RUNTIME_CATALOG_TIME_SIZE];
    cJSON *call = runtime_catalog_format_time(runtime_now_milliseconds(), now) ? host_places_argument(context) : NULL;
    if (call != NULL && cJSON_AddStringToObject(call, "now", now) == NULL) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

/** updates.run, which checks every registered host and updates each method-installed application; prints {checks, applications, refused}. */
static int update_everything(const host_context *context) {
    cJSON *refusal = NULL;
    cJSON *summary = host_call(runtime_updates_run, run_call(context), &refusal);
    if (summary == NULL) {
        return host_refuse(refusal);
    }
    host_print_json(summary);
    int refused = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(summary, "refused")) > 0;
    cJSON_Delete(summary);
    if (!refused) {
        return HOST_EXIT_DONE;
    }
    const char *parts[1] = {"not everything could be updated"};
    return host_refuse(host_refusal_of(parts, 1, UPDATE_FIX));
}

int host_command_update(int count, char **arguments) {
    (void)arguments;
    if (count != 0) {
        return host_misuse("update takes no arguments");
    }
    host_context context;
    cJSON *refusal = NULL;
    int code = host_open_context(&context, &refusal) ? update_everything(&context) : host_refuse(refusal);
    host_close_context(&context);
    return code;
}
