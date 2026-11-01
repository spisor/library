#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincred.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "secret_backend.h"
#include "secret_windows_fault.h"

#define LOGICAL_ROOT "spisor/"
#define CHUNK_ROOT "spisor-chunks/"
#define CHUNKED_MARKER L"spisor-chunked"
#define MAX_CHUNK_COUNT 1000UL
#define FIRST_GENERATION 1U
#define SECOND_GENERATION 2U
#define COUNT_TEXT_CAPACITY 24

static wchar_t *widen(const char *text) {
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    wchar_t *wide = length > 0 ? malloc((size_t)length * sizeof(wchar_t)) : NULL;
    if (wide != NULL && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, length) == 0) {
        free(wide);
        wide = NULL;
    }
    return wide;
}

static char *narrow(const wchar_t *text) {
    int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    char *utf8 = length > 0 ? malloc((size_t)length) : NULL;
    if (utf8 != NULL && WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, length, NULL, NULL) == 0) {
        free(utf8);
        utf8 = NULL;
    }
    return utf8;
}

static runtime_secret_answer answer_for(DWORD error) {
    switch (error) {
    case ERROR_NOT_FOUND:
        return RUNTIME_SECRET_ABSENT;
    case ERROR_CANCELLED:
    case ERROR_ACCESS_DENIED:
        return RUNTIME_SECRET_DECLINED;
    case ERROR_NO_SUCH_LOGON_SESSION:
    case ERROR_NOT_SUPPORTED:
    case ERROR_CALL_NOT_IMPLEMENTED:
        return RUNTIME_SECRET_UNAVAILABLE;
    default:
        return RUNTIME_SECRET_FAILED;
    }
}

static int has_logical_root(const char *entry) {
    return strncmp(entry, LOGICAL_ROOT, sizeof LOGICAL_ROOT - 1) == 0;
}

#ifdef SPISOR_SECRET_TEST_HOOKS
static long chunk_writes_allowed = -1;
static long base_reads_allowed = -1;

void runtime_secret_windows_allow_chunk_writes(long count) {
    chunk_writes_allowed = count;
}

void runtime_secret_windows_allow_base_reads(long count) {
    base_reads_allowed = count;
}

static int spend_allowance(long *allowed) {
    if (*allowed < 0) {
        return 0;
    }
    if (*allowed == 0) {
        return 1;
    }
    (*allowed)--;
    return 0;
}

static int chunk_writes_exhausted(void) {
    return spend_allowance(&chunk_writes_allowed);
}

static int base_reads_exhausted(void) {
    return spend_allowance(&base_reads_allowed);
}
#else
static int chunk_writes_exhausted(void) {
    return 0;
}

static int base_reads_exhausted(void) {
    return 0;
}
#endif

static wchar_t *chunk_target(const char *entry, unsigned generation, size_t number) {
    const char *below_root = entry + sizeof LOGICAL_ROOT - 1;
    size_t size = sizeof CHUNK_ROOT + strlen(below_root) + 2 * COUNT_TEXT_CAPACITY;
    char *name = malloc(size);
    wchar_t *target = NULL;
    if (name != NULL) {
        snprintf(name, size, CHUNK_ROOT "%s#%u#%zu", below_root, generation, number);
        target = widen(name);
    }
    free(name);
    return target;
}

static runtime_secret_answer write_credential(const wchar_t *target, const char *blob, size_t size,
                                              const wchar_t *comment) {
    CREDENTIALW credential;
    memset(&credential, 0, sizeof credential);
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = (LPWSTR)target;
    credential.Comment = (LPWSTR)comment;
    credential.CredentialBlobSize = (DWORD)size;
    credential.CredentialBlob = (LPBYTE)blob;
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&credential, 0) ? RUNTIME_SECRET_DONE : answer_for(GetLastError());
}

static runtime_secret_answer delete_credential(const wchar_t *target) {
    return CredDeleteW(target, CRED_TYPE_GENERIC, 0) ? RUNTIME_SECRET_DONE : answer_for(GetLastError());
}

static runtime_secret_answer read_credential(const wchar_t *target, PCREDENTIALW *credential) {
    return CredReadW(target, CRED_TYPE_GENERIC, 0, credential) ? RUNTIME_SECRET_DONE : answer_for(GetLastError());
}

static runtime_secret_answer write_named(const char *name, const char *blob, size_t size, const wchar_t *comment) {
    wchar_t *target = widen(name);
    if (target == NULL) {
        return RUNTIME_SECRET_FAILED;
    }
    runtime_secret_answer answer = write_credential(target, blob, size, comment);
    free(target);
    return answer;
}

static runtime_secret_answer write_chunk(const char *entry, unsigned generation, size_t number, const char *blob,
                                         size_t size) {
    wchar_t *target = chunk_target(entry, generation, number);
    if (target == NULL) {
        return RUNTIME_SECRET_FAILED;
    }
    runtime_secret_answer answer = chunk_writes_exhausted() ? RUNTIME_SECRET_FAILED
                                                           : write_credential(target, blob, size, NULL);
    free(target);
    return answer;
}

static runtime_secret_answer sweep_generation(const char *entry, unsigned generation) {
    for (size_t number = 1;; number++) {
        wchar_t *target = chunk_target(entry, generation, number);
        if (target == NULL) {
            return RUNTIME_SECRET_FAILED;
        }
        runtime_secret_answer answer = delete_credential(target);
        free(target);
        if (answer == RUNTIME_SECRET_ABSENT) {
            return RUNTIME_SECRET_DONE;
        }
        if (answer != RUNTIME_SECRET_DONE) {
            return answer;
        }
    }
}

static void sweep_every_generation(const char *entry) {
    sweep_generation(entry, FIRST_GENERATION);
    sweep_generation(entry, SECOND_GENERATION);
}

static size_t chunks_needed(size_t length) {
    return (length + CRED_MAX_CREDENTIAL_BLOB_SIZE - 1) / CRED_MAX_CREDENTIAL_BLOB_SIZE;
}

static unsigned other_generation(unsigned generation) {
    return generation == FIRST_GENERATION ? SECOND_GENERATION : FIRST_GENERATION;
}

static int is_chunked(const CREDENTIALW *credential) {
    return credential->Comment != NULL && wcscmp(credential->Comment, CHUNKED_MARKER) == 0;
}

static char *copy_blob_as_text(const CREDENTIALW *credential) {
    char *text = malloc((size_t)credential->CredentialBlobSize + 1);
    if (text != NULL) {
        if (credential->CredentialBlobSize > 0) {
            memcpy(text, credential->CredentialBlob, credential->CredentialBlobSize);
        }
        text[credential->CredentialBlobSize] = '\0';
    }
    return text;
}

static int parse_chunk_layout(const CREDENTIALW *credential, unsigned *generation, size_t *count) {
    char *text = copy_blob_as_text(credential);
    char *separator = NULL;
    char *end = NULL;
    unsigned long parsed_generation = text != NULL ? strtoul(text, &separator, 10) : 0;
    unsigned long parsed_count = separator != NULL && *separator == ':' ? strtoul(separator + 1, &end, 10) : 0;
    int valid = end != NULL && *end == '\0' && parsed_count >= 1 && parsed_count <= MAX_CHUNK_COUNT &&
                (parsed_generation == FIRST_GENERATION || parsed_generation == SECOND_GENERATION);
    *generation = valid ? (unsigned)parsed_generation : 0;
    *count = valid ? (size_t)parsed_count : 0;
    free(text);
    return valid;
}

/**
 * Answers DONE with the generation the stored value is chunked under, or the second one when there is
 * none (absent or whole), so the first is next. Any other answer means the live generation is unknown,
 * and the caller must not touch a chunk.
 */
static runtime_secret_answer generation_in_use(const char *entry, unsigned *generation) {
    wchar_t *target = widen(entry);
    PCREDENTIALW base = NULL;
    size_t count = 0;
    runtime_secret_answer answer = RUNTIME_SECRET_FAILED;
    *generation = SECOND_GENERATION;
    if (target == NULL) {
        return answer;
    }
    answer = base_reads_exhausted() ? RUNTIME_SECRET_FAILED : read_credential(target, &base);
    free(target);
    if (answer == RUNTIME_SECRET_DONE) {
        if (!is_chunked(base) || !parse_chunk_layout(base, generation, &count)) {
            *generation = SECOND_GENERATION;
        }
        CredFree(base);
    }
    return answer == RUNTIME_SECRET_ABSENT ? RUNTIME_SECRET_DONE : answer;
}

static runtime_secret_answer write_all_chunks(const char *entry, unsigned generation, const char *value,
                                              size_t length) {
    size_t count = chunks_needed(length);
    for (size_t number = 1; number <= count; number++) {
        size_t offset = (number - 1) * CRED_MAX_CREDENTIAL_BLOB_SIZE;
        size_t size = length - offset < CRED_MAX_CREDENTIAL_BLOB_SIZE ? length - offset : CRED_MAX_CREDENTIAL_BLOB_SIZE;
        runtime_secret_answer answer = write_chunk(entry, generation, number, value + offset, size);
        if (answer != RUNTIME_SECRET_DONE) {
            return answer;
        }
    }
    return RUNTIME_SECRET_DONE;
}

/**
 * @implNote A value that fits one credential blob (CRED_MAX_CREDENTIAL_BLOB_SIZE bytes of UTF-8) is
 * the blob of "spisor/<plugin>/<name>" itself. A longer one is cut into blobs of that size stored
 * as "spisor-chunks/<plugin>/<name>#<generation>#<n>", and the base entry then carries
 * "<generation>:<count>" as text with its comment set to CHUNKED_MARKER, which is what tells it
 * apart from a value that happens to look like that. Chunks live under their own root so that
 * listing "spisor/..." answers logical names only, a logical name containing "#" cannot be mistaken
 * for a chunk, and deleting a logical entry can find its chunks.
 * The generation alternates between 1 and 2 on every chunked rewrite. All new chunks are written
 * under the generation NOT in use, the base entry is written last, and only then is the old
 * generation deleted, so a rewrite that fails at any point leaves the previous value whole and
 * readable. A failed sweep is ignored: its orphans are always in one of the two generations, and
 * every later set and delete of that name clears both. A chunk may end inside a UTF-8 sequence;
 * only the joined value is text.
 */
static runtime_secret_answer write_chunked(const char *entry, const char *value, size_t length) {
    unsigned generation = SECOND_GENERATION;
    char layout[2 * COUNT_TEXT_CAPACITY];
    runtime_secret_answer answer = generation_in_use(entry, &generation);
    if (answer != RUNTIME_SECRET_DONE) {
        return answer;
    }
    generation = other_generation(generation);
    sweep_generation(entry, generation);
    answer = write_all_chunks(entry, generation, value, length);
    if (answer == RUNTIME_SECRET_DONE) {
        snprintf(layout, sizeof layout, "%u:%zu", generation, chunks_needed(length));
        answer = write_named(entry, layout, strlen(layout), CHUNKED_MARKER);
    }
    if (answer != RUNTIME_SECRET_DONE) {
        sweep_generation(entry, generation);
        return answer;
    }
    sweep_generation(entry, other_generation(generation));
    return RUNTIME_SECRET_DONE;
}

static runtime_secret_answer write_whole(const char *entry, const char *value, size_t length) {
    runtime_secret_answer answer = write_named(entry, value, length, NULL);
    if (answer == RUNTIME_SECRET_DONE) {
        sweep_every_generation(entry);
    }
    return answer;
}

static runtime_secret_answer append_chunk(const char *entry, unsigned generation, size_t number, char **value,
                                          size_t *length) {
    wchar_t *target = chunk_target(entry, generation, number);
    PCREDENTIALW chunk = NULL;
    if (target == NULL) {
        return RUNTIME_SECRET_FAILED;
    }
    runtime_secret_answer answer = read_credential(target, &chunk);
    free(target);
    if (answer == RUNTIME_SECRET_ABSENT) {
        return RUNTIME_SECRET_FAILED;
    }
    if (answer != RUNTIME_SECRET_DONE) {
        return answer;
    }
    char *grown = realloc(*value, *length + chunk->CredentialBlobSize + 1);
    if (grown == NULL) {
        CredFree(chunk);
        return RUNTIME_SECRET_FAILED;
    }
    memcpy(grown + *length, chunk->CredentialBlob, chunk->CredentialBlobSize);
    *length += chunk->CredentialBlobSize;
    grown[*length] = '\0';
    *value = grown;
    CredFree(chunk);
    return RUNTIME_SECRET_DONE;
}

static runtime_secret_answer join_chunks(const char *entry, unsigned generation, size_t count, char **value) {
    size_t length = 0;
    *value = calloc(1, 1);
    for (size_t number = 1; *value != NULL && number <= count; number++) {
        runtime_secret_answer answer = append_chunk(entry, generation, number, value, &length);
        if (answer != RUNTIME_SECRET_DONE) {
            free(*value);
            *value = NULL;
            return answer;
        }
    }
    return *value != NULL ? RUNTIME_SECRET_DONE : RUNTIME_SECRET_FAILED;
}

static runtime_secret_answer value_of(const char *entry, const CREDENTIALW *base, char **value) {
    unsigned generation = 0;
    size_t count = 0;
    if (!is_chunked(base)) {
        *value = copy_blob_as_text(base);
        return *value != NULL ? RUNTIME_SECRET_DONE : RUNTIME_SECRET_FAILED;
    }
    if (!parse_chunk_layout(base, &generation, &count)) {
        return RUNTIME_SECRET_FAILED;
    }
    return join_chunks(entry, generation, count, value);
}

int runtime_secret_store_available(void) {
    DWORD count = 0;
    PCREDENTIALW *credentials = NULL;
    if (CredEnumerateW(L"spisor-availability-probe-*", 0, &count, &credentials)) {
        CredFree(credentials);
        return 1;
    }
    return GetLastError() == ERROR_NOT_FOUND;
}

runtime_secret_answer runtime_secret_store_get(const char *entry, char **value) {
    wchar_t *target = has_logical_root(entry) ? widen(entry) : NULL;
    PCREDENTIALW base = NULL;
    *value = NULL;
    if (target == NULL) {
        return RUNTIME_SECRET_FAILED;
    }
    runtime_secret_answer answer = read_credential(target, &base);
    free(target);
    if (answer == RUNTIME_SECRET_DONE) {
        answer = value_of(entry, base, value);
        CredFree(base);
    }
    return answer;
}

runtime_secret_answer runtime_secret_store_set(const char *entry, const char *value) {
    size_t length = strlen(value);
    if (!has_logical_root(entry)) {
        return RUNTIME_SECRET_FAILED;
    }
    if (length > MAX_CHUNK_COUNT * CRED_MAX_CREDENTIAL_BLOB_SIZE) {
        return RUNTIME_SECRET_FAILED;
    }
    return length <= CRED_MAX_CREDENTIAL_BLOB_SIZE ? write_whole(entry, value, length)
                                                   : write_chunked(entry, value, length);
}

runtime_secret_answer runtime_secret_store_delete(const char *entry) {
    wchar_t *target = has_logical_root(entry) ? widen(entry) : NULL;
    if (target == NULL) {
        return RUNTIME_SECRET_FAILED;
    }
    runtime_secret_answer base_answer = delete_credential(target);
    free(target);
    if (base_answer != RUNTIME_SECRET_DONE && base_answer != RUNTIME_SECRET_ABSENT) {
        return base_answer;
    }
    sweep_every_generation(entry);
    return base_answer;
}

static runtime_secret_answer collect_names(PCREDENTIALW *credentials, DWORD count, const char *prefix,
                                           runtime_secret_names *names) {
    names->names = calloc(count > 0 ? count : 1, sizeof(char *));
    if (names->names == NULL) {
        return RUNTIME_SECRET_FAILED;
    }
    for (DWORD index = 0; index < count; index++) {
        char *name = narrow(credentials[index]->TargetName);
        if (name == NULL) {
            runtime_free_secret_names(names);
            return RUNTIME_SECRET_FAILED;
        }
        if (strncmp(name, prefix, strlen(prefix)) == 0 && strncmp(name, CHUNK_ROOT, sizeof CHUNK_ROOT - 1) != 0) {
            names->names[names->count++] = name;
        } else {
            free(name);
        }
    }
    return RUNTIME_SECRET_DONE;
}

runtime_secret_answer runtime_secret_store_list(const char *prefix, runtime_secret_names *names) {
    size_t filter_size = strlen(prefix) + 2;
    char *filter_text = malloc(filter_size);
    wchar_t *filter = NULL;
    DWORD count = 0;
    PCREDENTIALW *credentials = NULL;
    names->names = NULL;
    names->count = 0;
    if (filter_text != NULL) {
        snprintf(filter_text, filter_size, "%s*", prefix);
        filter = widen(filter_text);
    }
    free(filter_text);
    if (filter == NULL) {
        return RUNTIME_SECRET_FAILED;
    }
    BOOL enumerated = CredEnumerateW(filter, 0, &count, &credentials);
    DWORD error = enumerated ? ERROR_SUCCESS : GetLastError();
    free(filter);
    if (!enumerated) {
        return error == ERROR_NOT_FOUND ? RUNTIME_SECRET_DONE : answer_for(error);
    }
    runtime_secret_answer answer = collect_names(credentials, count, prefix, names);
    CredFree(credentials);
    return answer;
}
