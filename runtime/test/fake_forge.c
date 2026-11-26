#include "fake_forge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "identity_naula.h"
#include "naula/memory.h"
#include "naula/provider.h"

#define REPO_LIMIT 64
#define TEXT_LIMIT 32
#define TEXT_SIZE 2048

static fake_repo repos[REPO_LIMIT];
static size_t repo_count;
static char texts[TEXT_LIMIT][TEXT_SIZE];
static size_t text_count;
static unsigned calls;
static unsigned calls_with_token;
static char last_token[128];

/** Counts a call and the token it carried, so a test sees where a token went. */
static void note(const char *token) {
    calls++;
    if (token != NULL) {
        calls_with_token++;
        snprintf(last_token, sizeof last_token, "%s", token);
    }
}

unsigned fake_forge_calls(void) {
    return calls;
}

unsigned fake_forge_calls_with_token(const char *token) {
    return calls_with_token > 0 && strcmp(last_token, token) == 0 ? calls_with_token : 0;
}

void fake_forge_reset(void) {
    memset(repos, 0, sizeof repos);
    repo_count = 0;
    text_count = 0;
    calls = 0;
    calls_with_token = 0;
    last_token[0] = '\0';
}

fake_repo *fake_forge_add(uint64_t number, const char *account, const char *name, const char *file) {
    if (repo_count == REPO_LIMIT) {
        fprintf(stderr, "the fake forge holds at most %d repos\n", REPO_LIMIT);
        abort();
    }
    fake_repo *repo = &repos[repo_count++];
    memset(repo, 0, sizeof *repo);
    repo->number = number;
    repo->account = account;
    repo->name = name;
    repo->file = file;
    return repo;
}

const char *fake_forge_file(const char *name, const char *homes) {
    if (text_count == TEXT_LIMIT) {
        fprintf(stderr, "the fake forge holds at most %d files\n", TEXT_LIMIT);
        abort();
    }
    char *text = texts[text_count++];
    snprintf(text, TEXT_SIZE, "{\"format\":2,\"name\":\"%s\",\"importName\":\"plugin\",\"homes\":[%s]}", name, homes);
    return text;
}

/** The live repo of a number; a gone one answers NOT_FOUND, as a provider answers a deleted repo. */
static fake_repo *by_number(uint64_t number) {
    for (size_t index = repo_count; index > 0; index--) {
        if (repos[index - 1].number == number) {
            return &repos[index - 1];
        }
    }
    return NULL;
}

static naula_status refuse(naula_refusal *refusal, naula_status status, const char *code) {
    naula_refuse(refusal, code, "fix the fake forge", "the fake forge answered %s", code);
    return status;
}

static naula_status reach(const fake_repo *repo, naula_refusal *refusal) {
    if (repo == NULL || repo->gone) {
        return refuse(refusal, NAULA_NOT_FOUND, "not_found");
    }
    return repo->refused_with != NULL ? refuse(refusal, NAULA_REFUSED, repo->refused_with) : NAULA_OK;
}

static const char *root_of(const fake_repo *repo) {
    return repo->root == NULL ? FAKE_ROOT : repo->root;
}

static naula_status describe(void *context, uint32_t *supported, int *anonymous_lookup) {
    (void)context;
    *supported = NAULA_OP_LOOKUP | NAULA_OP_FIND | NAULA_OP_READ_FILE | NAULA_OP_CONTAINS | NAULA_OP_PARENT_COUNT;
    *anonymous_lookup = 1;
    return NAULA_OK;
}

static naula_status lookup(void *context, const char *host, uint64_t number, const char *token,
                           naula_repository *out, naula_refusal *refusal) {
    (void)context;
    (void)host;
    note(token);
    fake_repo *repo = by_number(number);
    naula_status status = reach(repo, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    repo->lookups++;
    int renamed = repo->renamed_to != NULL && repo->lookups > repo->lookups_before_rename;
    uint32_t size = out->size;
    memset(out, 0, sizeof *out);
    out->size = size;
    out->number = number;
    out->fork_parent = repo->fork_parent;
    out->account = naula_memory_duplicate_text(renamed ? repo->renamed_to : repo->account);
    out->name = naula_memory_duplicate_text(repo->name);
    out->default_branch = naula_memory_duplicate_text("main");
    if (out->account == NULL || out->name == NULL || out->default_branch == NULL) {
        naula_repository_release(out);
        return NAULA_OUT_OF_MEMORY;
    }
    return NAULA_OK;
}

static naula_status find(void *context, const char *host, const char *account, const char *name, const char *token,
                         uint64_t *number, naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)refusal;
    note(token);
    *number = 0;
    for (size_t index = 0; index < repo_count; index++) {
        const fake_repo *repo = &repos[index];
        if (!repo->gone && strcmp(repo->account, account) == 0 && strcmp(repo->name, name) == 0) {
            *number = repo->number;
        }
    }
    return NAULA_OK;
}

static naula_status read_file(void *context, const char *host, uint64_t number, const char *ref, const char *path,
                              const char *token, uint8_t **bytes, size_t *length, naula_refusal *refusal) {
    (void)context;
    (void)host;
    note(token);
    *bytes = NULL;
    *length = 0;
    const fake_repo *repo = by_number(number);
    naula_status status = reach(repo, refusal);
    if (status != NAULA_OK) {
        return status;
    }
    if (repo->file == NULL || ref == NULL || strcmp(ref, "development") != 0 || strcmp(path, "repo.json") != 0) {
        return refuse(refusal, NAULA_REFUSED, "missing_file_or_ref");
    }
    *length = strlen(repo->file);
    *bytes = naula_memory_allocate(*length + 1);
    if (*bytes == NULL) {
        *length = 0;
        return NAULA_OUT_OF_MEMORY;
    }
    memcpy(*bytes, repo->file, *length + 1);
    return NAULA_OK;
}

static naula_status contains(void *context, const char *host, uint64_t number, const char *ref, const char *commit,
                             const char *token, int *contained, naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)ref;
    note(token);
    *contained = 0;
    const fake_repo *repo = by_number(number);
    naula_status status = reach(repo, refusal);
    if (status == NAULA_OK) {
        *contained = strcmp(commit, root_of(repo)) == 0;
    }
    return status;
}

static naula_status parent_count(void *context, const char *host, uint64_t number, const char *commit,
                                 const char *token, size_t *count, naula_refusal *refusal) {
    (void)context;
    (void)host;
    note(token);
    *count = 0;
    const fake_repo *repo = by_number(number);
    naula_status status = reach(repo, refusal);
    if (status == NAULA_OK && strcmp(commit, root_of(repo)) != 0) {
        return refuse(refusal, NAULA_REFUSED, "missing_commit");
    }
    return status;
}

static naula_status list_repos(void *context, const char *host, const char *account, const char *token,
                               naula_repository **out, size_t *count, naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)account;
    (void)token;
    *out = NULL;
    *count = 0;
    return refuse(refusal, NAULA_REFUSED, "unsupported_operation");
}

static naula_status release_index(void *context, const char *host, uint64_t number, const char *tag,
                                  const char *token, uint8_t **bytes, size_t *length, naula_refusal *refusal) {
    (void)context;
    (void)host;
    (void)number;
    (void)tag;
    (void)token;
    *bytes = NULL;
    *length = 0;
    return refuse(refusal, NAULA_REFUSED, "unsupported_operation");
}

/** A reader only: its size ends at the first shape, so the registry asks it for no writes. */
static const naula_provider provider = {.size = (uint32_t)(offsetof(naula_provider, release_index) +
                                                         sizeof(((const naula_provider *)0)->release_index)),
                                        .word = "forge",
                                        .describe = describe,
                                        .lookup = lookup,
                                        .find = find,
                                        .read_file = read_file,
                                        .contains = contains,
                                        .parent_count = parent_count,
                                        .list_repos = list_repos,
                                        .release_index = release_index};

void fake_forge_install(void) {
    static int installed;
    if (installed) {
        return;
    }
    naula_refusal refusal;
    memset(&refusal, 0, sizeof refusal);
    refusal.size = sizeof refusal;
    if (runtime_identity_register_provider(&provider, &refusal) != NAULA_OK) {
        fprintf(stderr, "the fake forge could not be registered: %s\n", refusal.detail);
        abort();
    }
    installed = 1;
}
