#include <stddef.h>
#include <stdio.h>

#include <weivel/core.h>
#include <weivel/services.h>
#include <weivel/value.h>

#include "spisor/status.h"
#include "spisor/value.h"

/**
 * @file
 * spisor's status, refusal and value tree restate weivel's, so a library's public headers include no weivel header
 * while its definitions are bound by weivel's description of them. Every size, offset and number below is compared at
 * compile time, so a restatement that drifts from weivel's does not build.
 */

#define SAME_SIZE(spisor_type, weivel_type) \
    _Static_assert(sizeof(spisor_type) == sizeof(weivel_type), #spisor_type " is not the size of " #weivel_type)

#define SAME_MEMBER(spisor_type, weivel_type, member)                                                       \
    _Static_assert(offsetof(spisor_type, member) == offsetof(weivel_type, member) &&                        \
                       sizeof(((spisor_type *)0)->member) == sizeof(((weivel_type *)0)->member),            \
                   #spisor_type "." #member " is not where or the size " #weivel_type "." #member " is")

#define SAME_NUMBER(spisor_name, weivel_name) \
    _Static_assert((long long)(spisor_name) == (long long)(weivel_name), #spisor_name " is not " #weivel_name)

SAME_SIZE(spisor_status, weivel_status);
SAME_NUMBER(SPISOR_OK, WEIVEL_STATUS_OK);
SAME_NUMBER(SPISOR_NONE, WEIVEL_STATUS_NONE);
SAME_NUMBER(SPISOR_MALFORMED, WEIVEL_STATUS_MALFORMED);
SAME_NUMBER(SPISOR_REFUSED, WEIVEL_STATUS_REFUSED);
SAME_NUMBER(SPISOR_OUT_OF_MEMORY, WEIVEL_STATUS_OUT_OF_MEMORY);

SAME_SIZE(spisor_refusal, weivel_standard_service_refusal);
SAME_MEMBER(spisor_refusal, weivel_standard_service_refusal, size);
SAME_MEMBER(spisor_refusal, weivel_standard_service_refusal, detail);
SAME_MEMBER(spisor_refusal, weivel_standard_service_refusal, fix);
SAME_MEMBER(spisor_refusal, weivel_standard_service_refusal, storage);

SAME_NUMBER(SPISOR_VALUE_DEPTH_LIMIT, WEIVEL_STANDARD_VALUE_DEPTH_LIMIT);
SAME_SIZE(spisor_value_kind, weivel_standard_value_kind);
SAME_NUMBER(SPISOR_VALUE_NONE, WEIVEL_STANDARD_VALUE_NONE);
SAME_NUMBER(SPISOR_VALUE_NULL, WEIVEL_STANDARD_VALUE_NULL);
SAME_NUMBER(SPISOR_VALUE_BOOLEAN, WEIVEL_STANDARD_VALUE_BOOLEAN);
SAME_NUMBER(SPISOR_VALUE_NUMBER, WEIVEL_STANDARD_VALUE_NUMBER);
SAME_NUMBER(SPISOR_VALUE_TEXT, WEIVEL_STANDARD_VALUE_TEXT);
SAME_NUMBER(SPISOR_VALUE_LIST, WEIVEL_STANDARD_VALUE_LIST);
SAME_NUMBER(SPISOR_VALUE_RECORD, WEIVEL_STANDARD_VALUE_RECORD);

SAME_SIZE(spisor_value, weivel_standard_value);
SAME_MEMBER(spisor_value, weivel_standard_value, kind);
SAME_MEMBER(spisor_value, weivel_standard_value, boolean);
SAME_MEMBER(spisor_value, weivel_standard_value, number);
SAME_MEMBER(spisor_value, weivel_standard_value, text);
SAME_MEMBER(spisor_value, weivel_standard_value, length);
SAME_MEMBER(spisor_value, weivel_standard_value, items);
SAME_MEMBER(spisor_value, weivel_standard_value, entries);
SAME_MEMBER(spisor_value, weivel_standard_value, count);

SAME_SIZE(spisor_entry, weivel_standard_entry);
SAME_MEMBER(spisor_entry, weivel_standard_entry, key);
SAME_MEMBER(spisor_entry, weivel_standard_entry, key_length);
SAME_MEMBER(spisor_entry, weivel_standard_entry, value);

SAME_SIZE(spisor_value_tree, weivel_standard_value_tree);
SAME_MEMBER(spisor_value_tree, weivel_standard_value_tree, size);
SAME_MEMBER(spisor_value_tree, weivel_standard_value_tree, root);
SAME_MEMBER(spisor_value_tree, weivel_standard_value_tree, storage);

int main(void) {
    printf("restatement_test: every restated size, offset and number is weivel's\n");
    return 0;
}
