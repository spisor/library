#ifndef RUNTIME_JSON_KEY_CHANGE_H
#define RUNTIME_JSON_KEY_CHANGE_H

#include "cJSON.h"
#include "change_kinds.h"

/**
 * Why a json-key change cannot be recorded, or NULL when it can. A json-key change is
 * { kind: "json-key", file, key: [segment, ...], value, create? }: the member at key is set to value
 * in place (json_edit.h), every byte it does not own kept, and with create the file is made when it
 * is missing.
 */
const char *runtime_json_key_defect(const cJSON *change);

/**
 * As runtime_change_prepare, for a json-key. Besides file, key and value the record keeps what the
 * reversal needs: "previous", the value that was replaced, or "createdFrom", the depth of the first
 * member added; "before", the whole file's bytes beforehand when they are UTF-8 without a NUL;
 * "after", the digest of the bytes the change wrote; and "created" when it made the file.
 */
runtime_change_step runtime_json_key_prepare(const cJSON *change, cJSON **record, runtime_change_note *note);

/** As runtime_change_perform, for a json-key; record's "after" is refined to the bytes written. */
runtime_change_step runtime_json_key_perform(const cJSON *change, cJSON *record, runtime_change_note *note);

/** As runtime_change_is_on_disk, for a json-key. */
int runtime_json_key_is_on_disk(const cJSON *record);

/**
 * As runtime_change_reverse, for a json-key. A file still holding exactly the bytes the change wrote
 * gets its "before" bytes back, or is deleted when the change made it. Otherwise only the change's own
 * member is undone in place, and every byte around it stays as it now is: the replaced value's own
 * bytes are put back, or the added member is removed with each object it added that is left empty.
 * DRIFTED, with the file left, when the member no longer holds the value the change set.
 */
runtime_change_reversal runtime_json_key_reverse(const cJSON *record, runtime_change_note *note);

#endif
