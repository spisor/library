#ifndef RUNTIME_SECRET_FILE_H
#define RUNTIME_SECRET_FILE_H

#include "cJSON.h"

typedef enum runtime_secret_file_answer {
    RUNTIME_SECRET_FILE_DONE,
    RUNTIME_SECRET_FILE_UNREADABLE,
    RUNTIME_SECRET_FILE_UNWRITABLE,
    RUNTIME_SECRET_FILE_OUT_OF_MEMORY
} runtime_secret_file_answer;

/**
 * The secrets file's entries, the JSON object {"<entry>": "<value>"}, freed with cJSON_Delete; an
 * absent file is an empty object. NULL when the file exists but is not one readable JSON object
 * (UNREADABLE), so a caller never writes over content it could not read, or when memory runs out.
 */
cJSON *runtime_read_secret_file(const char *path, runtime_secret_file_answer *answer);

/**
 * Writes entries over the file, creating its directory when absent, readable by its owner alone
 * from the moment it exists.
 *
 * @implNote POSIX gives the file mode 0600 through spisor_replace_private_file. Windows gives it the
 * ACL its directory passes on, which under %LOCALAPPDATA% already admits only the user (with SYSTEM
 * and administrators); a directory named by SPISOR_SECRETS keeps the ACL its owner chose.
 */
runtime_secret_file_answer runtime_write_secret_file(const char *path, const cJSON *entries);

/** The value the file holds for entry, freed with free; NULL with DONE when it holds none. */
char *runtime_get_file_secret(const char *path, const char *entry, runtime_secret_file_answer *answer);

runtime_secret_file_answer runtime_put_file_secret(const char *path, const char *entry, const char *value);

/** Removes entry; the file is rewritten only when it held it, and never created. */
runtime_secret_file_answer runtime_remove_file_secret(const char *path, const char *entry);

/** Removes every entry whose name starts with prefix; rewritten only when one was removed. */
runtime_secret_file_answer runtime_remove_file_secrets_under(const char *path, const char *prefix);

#endif
