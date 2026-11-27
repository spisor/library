#include "scope_manifest.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "config_conversion.h"
#include "spisor/definition.h"
#include "spisor/json_file.h"

#define DETAIL_BYTES 4096

static spisor_status refuse_manifest(spisor_refusal *refusal, const char *path, const char *problem,
                                     const char *fix) {
    char detail[DETAIL_BYTES];
    snprintf(detail, sizeof detail, "the manifest at %s %s", path, problem);
    return spisor_refuse_as(refusal, SPISOR_REFUSED, detail, fix);
}

static spisor_status refuse_setting(spisor_refusal *refusal, const char *path, const char *key, const char *problem,
                                    const char *fix) {
    char detail[DETAIL_BYTES];
    snprintf(detail, sizeof detail, "the manifest at %s declares the setting \"%s\" %s", path, key, problem);
    return spisor_refuse_as(refusal, SPISOR_REFUSED, detail, fix);
}

static spisor_status refuse_layer(spisor_refusal *refusal, const char *path, const char *key, const cJSON *layer) {
    static const char fix[] =
        "declare its layer as application or portable, or leave the layer out to keep it in the portable layer";
    char detail[DETAIL_BYTES];
    if (!cJSON_IsString(layer)) {
        return refuse_setting(refusal, path, key, "with a layer that is not text", fix);
    }
    snprintf(detail, sizeof detail,
             "the manifest at %s declares the setting \"%s\" in the layer \"%s\", which runtime does not know", path,
             key, layer->valuestring);
    return spisor_refuse_as(refusal, SPISOR_REFUSED, detail, fix);
}

static spisor_status refuse_type(spisor_refusal *refusal, const char *path, const char *key, const cJSON *type) {
    static const char fix[] = "declare its type as one of text, secret, number, toggle, choice, multi-choice, path, "
                              "list or key-value, or leave the type out for text";
    char detail[DETAIL_BYTES];
    if (!cJSON_IsString(type)) {
        return refuse_setting(refusal, path, key, "with a type that is not text", fix);
    }
    snprintf(detail, sizeof detail,
             "the manifest at %s declares the setting \"%s\" with the type \"%s\", which runtime does not know", path,
             key, type->valuestring);
    return spisor_refuse_as(refusal, SPISOR_REFUSED, detail, fix);
}

/** The declaration's type and choices checked: OK, or REFUSED naming the first that runtime cannot read. */
static spisor_status check_field(const cJSON *declaration, const char *path, spisor_refusal *refusal) {
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(declaration, "type");
    if (type != NULL && !(cJSON_IsString(type) && runtime_field_kind_named(type->valuestring) != 0)) {
        return refuse_type(refusal, path, declaration->string, type);
    }
    if (!runtime_are_setting_choices(cJSON_GetObjectItemCaseSensitive(declaration, "choices"))) {
        return refuse_setting(refusal, path, declaration->string, "with choices that are not a list of texts",
                              "declare its choices as a list of texts, such as [\"light\", \"dark\"]");
    }
    return SPISOR_OK;
}

static spisor_status check_declaration(const cJSON *declaration, const char *path, spisor_refusal *refusal) {
    const cJSON *layer = cJSON_GetObjectItemCaseSensitive(declaration, "layer");
    if (!cJSON_IsObject(declaration)) {
        return refuse_setting(refusal, path, declaration->string, "as something other than a record",
                              "declare each setting as a record, such as {\"type\": \"text\"}");
    }
    if (layer != NULL && !(cJSON_IsString(layer) && runtime_is_settings_layer_name(layer->valuestring))) {
        return refuse_layer(refusal, path, declaration->string, layer);
    }
    return check_field(declaration, path, refusal);
}

static spisor_status check_settings(const cJSON *settings, const char *path, spisor_refusal *refusal) {
    const cJSON *declaration;
    if (settings == NULL) {
        return SPISOR_OK;
    }
    if (!cJSON_IsObject(settings)) {
        return refuse_manifest(refusal, path, "declares settings that are not a record",
                               "declare settings as a record of each setting's key to its declaration");
    }
    cJSON_ArrayForEach(declaration, settings) {
        spisor_status status = check_declaration(declaration, path, refusal);
        if (status != SPISOR_OK) {
            return status;
        }
    }
    return SPISOR_OK;
}

static spisor_status check_manifest(const runtime_scope_manifest *manifest, const char *path,
                                    spisor_refusal *refusal) {
    if (!cJSON_IsObject(manifest->manifest)) {
        return refuse_manifest(refusal, path, "is not a JSON object", "write the manifest as one JSON object");
    }
    if (manifest->id == NULL) {
        return refuse_manifest(refusal, path, "carries no id", "give the manifest its plugin id as the text id");
    }
    if (manifest->config != NULL && !cJSON_IsObject(manifest->config)) {
        return refuse_manifest(refusal, path, "declares a config that is not a record",
                               "declare config as a record, its defaults and name inside it, or leave it out");
    }
    return check_settings(manifest->settings, path, refusal);
}

static spisor_status refuse_unread(spisor_json_file_state state, const char *path, spisor_refusal *refusal) {
    if (state == SPISOR_JSON_FILE_OUT_OF_MEMORY) {
        return SPISOR_OUT_OF_MEMORY;
    }
    if (state == SPISOR_JSON_FILE_ABSENT) {
        return refuse_manifest(refusal, path, "does not exist", "pass the path of the plugin's manifest file");
    }
    return refuse_manifest(refusal, path, "cannot be read as JSON", "repair the manifest, then start the host again");
}

spisor_status runtime_read_scope_manifest(const char *path, runtime_scope_manifest *manifest,
                                          spisor_refusal *refusal) {
    spisor_json_file_state state = SPISOR_JSON_FILE_ABSENT;
    spisor_status status;
    memset(manifest, 0, sizeof *manifest);
    manifest->manifest = spisor_read_json_file(path, &state);
    if (manifest->manifest == NULL) {
        return refuse_unread(state, path, refusal);
    }
    manifest->id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(manifest->manifest, "id"));
    manifest->config = cJSON_GetObjectItemCaseSensitive(manifest->manifest, "config");
    manifest->settings = cJSON_GetObjectItemCaseSensitive(manifest->manifest, "settings");
    status = check_manifest(manifest, path, refusal);
    if (status != SPISOR_OK) {
        runtime_scope_manifest_free(manifest);
    }
    return status;
}

void runtime_scope_manifest_free(runtime_scope_manifest *manifest) {
    cJSON_Delete(manifest->manifest);
    memset(manifest, 0, sizeof *manifest);
}

spisor_status runtime_check_scope_plugin(const runtime_config_request *request, const char *manifest_path,
                                         spisor_refusal *refusal) {
    spisor_refusal checked = SPISOR_INIT(spisor_refusal);
    spisor_status status = runtime_config_check_request(request, &checked);
    char detail[DETAIL_BYTES];
    if (status != SPISOR_MALFORMED) {
        spisor_refusal_release(&checked);
        return status;
    }
    snprintf(detail, sizeof detail, "the manifest at %s is refused: %s", manifest_path,
             checked.detail == NULL ? "its plugin cannot be located" : checked.detail);
    status = spisor_refuse_as(refusal, SPISOR_REFUSED, detail, checked.fix);
    spisor_refusal_release(&checked);
    return status;
}
