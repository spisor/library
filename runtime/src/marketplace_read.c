#include "marketplace_read.h"
#include "allocation.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "prepared_undo.h"
#include "runtime/fetch.h"
#include "import_name.h"
#include "paths.h"
#include "json_service.h"
#include "marketplace_check.h"
#include "marketplace_sources.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"
#include "spisor/path.h"

#define MESSAGE_SIZE 2048
#define PROBLEM_SIZE 1024
#define BYTE_ORDER_MARK "\xEF\xBB\xBF"
#define FETCH_TIMEOUT_SECONDS 30
#define BUILD_FIX "build the copy from the list with marketplace build and serve the file it writes, not the list"

/**
 * @implNote The test build also accepts http:, which only a loopback fixture serves; the library refuses it, as the
 * fetch it calls does.
 */
static int is_readable_url(const char *url) {
#ifdef SPISOR_MARKETPLACE_TEST_HOOKS
    if (strncmp(url, "http:", 5) == 0) {
        return 1;
    }
#endif
    return runtime_marketplace_is_usable_url(url);
}


static cJSON *refuse(cJSON **refusal, const char *fix, const char *format, ...) {
    char message[MESSAGE_SIZE];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof message, format, arguments);
    va_end(arguments);
    *refusal = runtime_refusal(message, fix);
    return NULL;
}

static const char *text_of(const cJSON *object, const char *member) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, member);
    return cJSON_IsString(value) ? value->valuestring : NULL;
}

static int is_source_usable(const runtime_marketplace_read_request *order, cJSON **refusal) {
    if (order->source_id == NULL || !runtime_is_import_name(order->source_id)) {
        refuse(refusal, "pass the source as marketplace.sources answers it, with the id an import name",
               "the call carries no usable source");
        return 0;
    }
    if (order->url == NULL) {
        refuse(refusal, "ask the plugin that provides it for the copy and pass the answer as text",
               "%s has no url, so this library cannot fetch it", order->source_id);
        return 0;
    }
    if (!is_readable_url(order->url)) {
        refuse(refusal, "give the source a file: or https: url", "the url of source %s is not a file: or https: url",
               order->source_id);
        return 0;
    }
    return 1;
}

static int has_way_to_read(const runtime_marketplace_read_request *order, cJSON **refusal) {
    if (order->text == NULL && (order->destination == NULL || !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, order->destination))) {
        refuse(refusal, "pass text, the body already fetched, or destination, an absolute path to download into",
               "the call carries neither text nor a usable destination for %s", order->source_id);
        return 0;
    }
    spisor_file_presence presence = order->text == NULL ? spisor_probe_file(order->destination) : SPISOR_FILE_MISSING;
    if (presence == SPISOR_FILE_OUT_OF_MEMORY) {
        return 0;
    }
    if (presence == SPISOR_FILE_PRESENT) {
        refuse(refusal, "send a destination that does not exist; it is deleted once read, so only a file this call wrote",
               "the destination for %s already exists", order->source_id);
        return 0;
    }
    return 1;
}

static void refuse_fetch(const runtime_marketplace_read_request *order, const spisor_refusal *fetch_refusal,
                         cJSON **refusal) {
    refuse(refusal, fetch_refusal->fix != NULL ? fetch_refusal->fix : "check the url and try again",
           "%s could not be fetched: %s", order->source_id, fetch_refusal->detail);
}

/** Fetches the copy to its destination; 1 once stored, else 0 with *refusal set, or with none out of memory. */
static int fetch_copy(const runtime_marketplace_read_request *order, cJSON **refusal) {
    const runtime_fetch_definition *fetch = runtime_fetch_definition_of();
    runtime_fetch_get_argument argument = SPISOR_INIT(runtime_fetch_get_argument);
    runtime_fetch_get_answer answer = SPISOR_INIT(runtime_fetch_get_answer);
    spisor_refusal fetch_refusal = SPISOR_INIT(spisor_refusal);
    argument.url = order->url;
    argument.destination = order->destination;
    argument.limited = 1;
    argument.max_bytes = RUNTIME_MARKETPLACE_MAXIMUM_BYTES;
    argument.timeout_seconds = FETCH_TIMEOUT_SECONDS;
    spisor_status status = fetch->get(fetch->context, &argument, &answer, &fetch_refusal);
    if (status != SPISOR_OK && fetch_refusal.detail != NULL) {
        refuse_fetch(order, &fetch_refusal, refusal);
    }
    fetch->release_answer(fetch->context, &answer);
    fetch->release_refusal(fetch->context, &fetch_refusal);
    return status == SPISOR_OK;
}

/**
 * The bytes fetch's get stored at the destination, freed; NULL with *refusal set, or with none when memory
 * ran out.
 */
static char *fetched_text(const runtime_marketplace_read_request *order, size_t *length, cJSON **refusal) {
    if (!fetch_copy(order, refusal)) {
        return NULL;
    }
    spisor_read_outcome outcome = SPISOR_READ_FAILED;
    char *bytes = (char *)spisor_read_file_reporting(order->destination, length, &outcome);
    spisor_remove_file_in_place(order->destination);
    if (bytes == NULL && outcome != SPISOR_READ_OUT_OF_MEMORY) {
        refuse(refusal, "try again", "%s was fetched but the download could not be read", order->source_id);
    }
    return bytes;
}

static int is_web_page(const char *text, size_t length) {
    size_t at = 0;
    while (at < length && isspace((unsigned char)text[at])) {
        at++;
    }
    return at < length && text[at] == '<';
}

/** The parsed copy; NULL with *refusal set, or with none when memory ran out. */
static cJSON *parsed_copy(const runtime_marketplace_read_request *order, const char *text, size_t length,
                          cJSON **refusal) {
    size_t skipped = length >= 3 && memcmp(text, BYTE_ORDER_MARK, 3) == 0 ? 3 : 0;
    int out_of_memory = 0;
    cJSON *document = spisor_parse_json_reporting((const uint8_t *)text + skipped, length - skipped, &out_of_memory);
    if (document != NULL || out_of_memory) {
        return document;
    }
    if (is_web_page(text + skipped, length - skipped)) {
        refuse(refusal, "point the url at the generated copy file itself, such as its raw address, not at a web page",
               "%s answered a web page instead of a marketplace copy", order->source_id);
    } else {
        refuse(refusal, "serve the copy marketplace build writes, which is one JSON document in UTF-8",
               "%s did not answer JSON", order->source_id);
    }
    return NULL;
}

static int is_list_instead_of_copy(const cJSON *document) {
    char problem[PROBLEM_SIZE];
    return runtime_marketplace_check_list(document, problem, sizeof problem);
}

/** 1 for a copy; 0 with a refusal, or without one when memory runs out. */
static int check_shape(const runtime_marketplace_read_request *order, const cJSON *document, cJSON **refusal) {
    char problem[PROBLEM_SIZE];
    int verdict = runtime_marketplace_check_copy(document, problem, sizeof problem);
    if (verdict != 0) {
        return verdict > 0;
    }
    int is_list = is_list_instead_of_copy(document);
    if (is_list < 0) {
        return 0;
    }
    if (is_list) {
        refuse(refusal, BUILD_FIX, "%s answered a marketplace list where a copy is expected", order->source_id);
    } else {
        refuse(refusal, "serve the copy marketplace build writes", "%s is not a marketplace copy: %s", order->source_id,
               problem);
    }
    return 0;
}

static int check_names_are_unique(const runtime_marketplace_read_request *order, const cJSON *document,
                                  cJSON **refusal) {
    const cJSON *entries = cJSON_GetObjectItemCaseSensitive(document, "entries");
    for (const cJSON *entry = entries->child; entry != NULL; entry = entry->next) {
        for (const cJSON *earlier = entries->child; earlier != entry; earlier = earlier->next) {
            if (strcmp(text_of(earlier, "name"), text_of(entry, "name")) == 0) {
                refuse(refusal, "build the copy again; two entries may not share a name once aliases apply",
                       "%s is not a usable marketplace copy: %s and %s are both named %s", order->source_id,
                       text_of(earlier, "id"), text_of(entry, "id"), text_of(entry, "name"));
                return 0;
            }
        }
    }
    return 1;
}

static cJSON *copy_answer(cJSON *document) {
    cJSON *answer = cJSON_CreateObject();
    if (answer == NULL || !cJSON_AddItemToObject(answer, "copy", document)) {
        cJSON_Delete(answer);
        cJSON_Delete(document);
        return NULL;
    }
    return answer;
}

static cJSON *interpreted(const runtime_marketplace_read_request *order, const char *text, size_t length,
                          cJSON **refusal) {
    if (length > RUNTIME_MARKETPLACE_MAXIMUM_BYTES) {
        return refuse(refusal, "serve a smaller copy, or list fewer entries in the marketplace",
                      "%s served more than the limit of %d bytes", order->source_id, RUNTIME_MARKETPLACE_MAXIMUM_BYTES);
    }
    cJSON *document = parsed_copy(order, text, length, refusal);
    if (document == NULL) {
        return NULL;
    }
    if (!check_shape(order, document, refusal) || !check_names_are_unique(order, document, refusal)) {
        cJSON_Delete(document);
        return NULL;
    }
    return copy_answer(document);
}

cJSON *runtime_marketplace_read(const runtime_marketplace_read_request *request, cJSON **refusal) {
    *refusal = NULL;
    if (!is_source_usable(request, refusal) || !has_way_to_read(request, refusal)) {
        return NULL;
    }
    if (request->text != NULL) {
        return interpreted(request, request->text, strlen(request->text), refusal);
    }
    size_t length = 0;
    char *fetched = fetched_text(request, &length, refusal);
    cJSON *answer = fetched == NULL ? NULL : interpreted(request, fetched, length, refusal);
    spisor_memory_release(fetched);
    return answer;
}
