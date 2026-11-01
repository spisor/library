#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincred.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "secret_backend.h"

#define LOGICAL_ROOT "spisor/"
#define CHUNK_ROOT "spisor-chunks/"
#define CHUNKED_MARKER L"spisor-chunked"
#define MAX_CHUNK_COUNT 100000UL
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

static wchar_t *chunk_target(const char *entry, size_t number) {
    const char *below_root = entry + sizeof LOGICAL_ROOT - 1;
    size_t size = sizeof CHUNK_ROOT + strlen(below_root) + COUNT_TEXT_CAPACITY;
    char *name = malloc(size);
    wchar_t *target = NULL;
    if (name != NULL) {
        snprintf(name, size, CHUNK_ROOT "%s#%zu", below_root, number);
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

static runtime_secret_answer write_chunk(const char *entry, size_t number, const char *blob, size_t size) {
    wchar_t *target = chunk_target(entry, number);
    if (target == NULL) {
        return RUNTIME_SECRET_FAILED;
    }
    runtime_secret_answer answer = write_credential(target, blob, size, NULL);
    free(target);
    return answer;
}

static runtime_secret_answer delete_chunks_from(const char *entry, size_t first) {
    for (size_t number = first;; number++) {
        wchar_t *target = chunk_target(entry, number);
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

static size_t chunks_needed(size_t length) {
    return (length + CRED_MAX_CREDENTIAL_BLOB_SIZE - 1) / CRED_MAX_CREDENTIAL_BLOB_SIZE;
}

/**
 * @implNote A value that fits one credential blob (CRED_MAX_CREDENTIAL_BLOB_SIZE bytes of UTF-8) is
 * the blob of "spisor/<plugin>/<name>" itself. A longer one is cut into blobs of that size stored
 * as "spisor-chunks/<plugin>/<name>#1", "#2", ..., and the base entry then carries the chunk count
 * as decimal text with its comment set to CHUNKED_MARKER, which is what tells it apart from a
 * value that happens to be digits. Chunks live under their own root so that listing "spisor/..."
 * answers logical names only, a logical name containing "#" cannot be mistaken for a chunk, and
 * deleting a logical entry can find its chunks. A chunk may end inside a UTF-8 sequence; only the
 * joined value is text.
 */
static runtime_secret_answer write_chunks_then_count(const char *entry, const char *value, size_t length) {
    size_t count = chunks_needed(length);
    char count_text[COUNT_TEXT_CAPACITY];
    for (size_t number = 1; number <= count; number++) {
        size_t offset = (number - 1) * CRED_MAX_CREDENTIAL_BLOB_SIZE;
        size_t size = length - offset < CRED_MAX_CREDENTIAL_BLOB_SIZE ? length - offset : CRED_MAX_CREDENTIAL_BLOB_SIZE;
        runtime_secret_answer answer = write_chunk(entry, number, value + offset, size);
        if (answer != RUNTIME_SECRET_DONE) {
            return answer;
        }
    }
    snprintf(count_text, sizeof count_text, "%zu", count);
    runtime_secret_answer answer = write_named(entry, count_text, strlen(count_text), CHUNKED_MARKER);
    return answer == RUNTIME_SECRET_DONE ? delete_chunks_from(entry, count + 1) : answer;
}

static runtime_secret_answer write_whole_then_drop_chunks(const char *entry, const char *value, size_t length) {
    runtime_secret_answer answer = write_named(entry, value, length, NULL);
    return answer == RUNTIME_SECRET_DONE ? delete_chunks_from(entry, 1) : answer;
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

static int parse_chunk_count(const CREDENTIALW *credential, size_t *count) {
    char *text = copy_blob_as_text(credential);
    char *end = NULL;
    unsigned long parsed = text != NULL ? strtoul(text, &end, 10) : 0;
    int valid = text != NULL && end != text && *end == '\0' && parsed >= 1 && parsed <= MAX_CHUNK_COUNT;
    *count = valid ? (size_t)parsed : 0;
    free(text);
    return valid;
}

static runtime_secret_answer append_chunk(const char *entry, size_t number, char **value, size_t *length) {
    wchar_t *target = chunk_target(entry, number);
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

static runtime_secret_answer join_chunks(const char *entry, size_t count, char **value) {
    size_t length = 0;
    *value = calloc(1, 1);
    for (size_t number = 1; *value != NULL && number <= count; number++) {
        runtime_secret_answer answer = append_chunk(entry, number, value, &length);
        if (answer != RUNTIME_SECRET_DONE) {
            free(*value);
            *value = NULL;
            return answer;
        }
    }
    return *value != NULL ? RUNTIME_SECRET_DONE : RUNTIME_SECRET_FAILED;
}

static runtime_secret_answer value_of(const char *entry, const CREDENTIALW *base, char **value) {
    size_t count = 0;
    if (!is_chunked(base)) {
        *value = copy_blob_as_text(base);
        return *value != NULL ? RUNTIME_SECRET_DONE : RUNTIME_SECRET_FAILED;
    }
    if (!parse_chunk_count(base, &count)) {
        return RUNTIME_SECRET_FAILED;
    }
    return join_chunks(entry, count, value);
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
    return length <= CRED_MAX_CREDENTIAL_BLOB_SIZE ? write_whole_then_drop_chunks(entry, value, length)
                                                   : write_chunks_then_count(entry, value, length);
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
    runtime_secret_answer chunks_answer = delete_chunks_from(entry, 1);
    return chunks_answer == RUNTIME_SECRET_DONE ? base_answer : chunks_answer;
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
        if (strncmp(name, prefix, strlen(prefix)) == 0) {
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
