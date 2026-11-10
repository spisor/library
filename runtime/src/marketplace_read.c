#include "marketplace_read.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "directory_tree.h"
#include "fetch.h"
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

typedef struct reading {
    const char *source_id;
    const char *url;
    const char *text;
    const char *destination;
} reading;

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

static int is_source_usable(const cJSON *source, reading *order, cJSON **refusal) {
    order->source_id = text_of(source, "id");
    order->url = text_of(source, "url");
    if (order->source_id == NULL || !runtime_is_import_name(order->source_id)) {
        refuse(refusal, "send source as {id, url} as marketplace.sources answers it, with the id an import name",
               "the argument carries no usable source");
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

static int has_way_to_read(const cJSON *argument, reading *order, cJSON **refusal) {
    order->text = text_of(argument, "text");
    order->destination = text_of(argument, "destination");
    if (order->text == NULL && (order->destination == NULL || !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, order->destination))) {
        refuse(refusal, "send text, the body already fetched, or destination, an absolute path to download into",
               "the argument carries neither text nor a usable destination for %s", order->source_id);
        return 0;
    }
    if (order->text == NULL && spisor_file_exists(order->destination)) {
        refuse(refusal, "send a destination that does not exist; it is deleted once read, so only a file this call wrote",
               "the destination for %s already exists", order->source_id);
        return 0;
    }
    return 1;
}

static cJSON *fetch_request(const reading *order) {
    cJSON *request = cJSON_CreateObject();
    if (request == NULL || cJSON_AddStringToObject(request, "url", order->url) == NULL ||
        cJSON_AddStringToObject(request, "destination", order->destination) == NULL ||
        cJSON_AddNumberToObject(request, "maxBytes", RUNTIME_MARKETPLACE_MAXIMUM_BYTES) == NULL ||
        cJSON_AddNumberToObject(request, "timeoutSeconds", FETCH_TIMEOUT_SECONDS) == NULL) {
        cJSON_Delete(request);
        return NULL;
    }
    return request;
}

static void refuse_fetch(const reading *order, const cJSON *fetch_refusal, cJSON **refusal) {
    const char *fix = text_of(fetch_refusal, "fix");
    refuse(refusal, fix != NULL ? fix : "check the url and try again", "%s could not be fetched: %s", order->source_id,
           text_of(fetch_refusal, "message"));
}

/** The bytes fetch.get stored at the destination, freed with free; NULL with *refusal set, or with none when memory ran out. */
static char *fetched_text(const reading *order, size_t *length, cJSON **refusal) {
    cJSON *fetch_refusal = NULL;
    cJSON *answer = runtime_call_or_refuse(runtime_fetch_get, fetch_request(order), &fetch_refusal);
    if (answer == NULL) {
        if (runtime_is_refusal(fetch_refusal)) {
            refuse_fetch(order, fetch_refusal, refusal);
        }
        cJSON_Delete(fetch_refusal);
        return NULL;
    }
    cJSON_Delete(answer);
    char *bytes = (char *)spisor_read_file(order->destination, length);
    runtime_remove_file_entry(order->destination);
    if (bytes == NULL) {
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

static cJSON *parsed_copy(const reading *order, const char *text, size_t length, cJSON **refusal) {
    size_t skipped = length >= 3 && memcmp(text, BYTE_ORDER_MARK, 3) == 0 ? 3 : 0;
    cJSON *document = spisor_parse_json((const uint8_t *)text + skipped, length - skipped);
    if (document != NULL) {
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

static int check_shape(const reading *order, const cJSON *document, cJSON **refusal) {
    char problem[PROBLEM_SIZE];
    if (runtime_marketplace_check_copy(document, problem, sizeof problem)) {
        return 1;
    }
    if (is_list_instead_of_copy(document)) {
        refuse(refusal, BUILD_FIX, "%s answered a marketplace list where a copy is expected", order->source_id);
    } else {
        refuse(refusal, "serve the copy marketplace build writes", "%s is not a marketplace copy: %s", order->source_id,
               problem);
    }
    return 0;
}

static int check_names_are_unique(const reading *order, const cJSON *document, cJSON **refusal) {
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
        return NULL;
    }
    return answer;
}

static cJSON *interpreted(const reading *order, const char *text, size_t length, cJSON **refusal) {
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

static cJSON *read_copy(const cJSON *argument, cJSON **refusal) {
    reading order;
    if (!is_source_usable(cJSON_GetObjectItemCaseSensitive(argument, "source"), &order, refusal) ||
        !has_way_to_read(argument, &order, refusal)) {
        return NULL;
    }
    if (order.text != NULL) {
        return interpreted(&order, order.text, strlen(order.text), refusal);
    }
    size_t length = 0;
    char *fetched = fetched_text(&order, &length, refusal);
    cJSON *answer = fetched == NULL ? NULL : interpreted(&order, fetched, length, refusal);
    free(fetched);
    return answer;
}

RUNTIME_EXPORT int32_t runtime_marketplace_read(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, read_copy);
}
