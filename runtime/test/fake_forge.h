#ifndef RUNTIME_TEST_FAKE_FORGE_H
#define RUNTIME_TEST_FAKE_FORGE_H

#include <stddef.h>
#include <stdint.h>

/** The root commit every fake repo holds, and the id it gives them. */
#define FAKE_ROOT "dd92836cca7c292258635112ecfe1acb7b4f6ecd"
#define FAKE_ID "git:" FAKE_ROOT

/** One home of a repo.json the fake answers: an account on the fake provider. */
#define FAKE_HOME(account) "{\"provider\":\"forge\",\"account\":\"" account "\"}"

/**
 * A repo the fake provider `forge` answers by number: its account and name, the repo.json its development branch
 * holds, the number it was forked from (0 for none), and whether it is gone, which answers NOT_FOUND by number and
 * nothing by name. `root` is the root commit it holds, FAKE_ROOT when NULL; `refused_with`, when set, is the code
 * every call by number is refused with, as a host that rate limits or wants a token. `index` and `index_url` are the latest release's
 * weivel-index.json and the address it is offered at, NULL for a repo with no such asset; `times_out` makes every call
 * by number time out, as a host that cannot be heard.
 */
typedef struct fake_repo {
    uint64_t number;
    const char *account;
    const char *name;
    const char *file;
    uint64_t fork_parent;
    int gone;
    const char *root;
    const char *refused_with;
    const char *index;
    const char *index_url;
    int times_out;
} fake_repo;

/** Registers the fake provider `forge` with runtime's providers, once per process. */
void fake_forge_install(void);

/** Forgets every repo and file the fake holds. */
void fake_forge_reset(void);

/**
 * Gives every repo added from now on a latest release offering `index` at `url`, which outlive those repos; NULL for
 * repos with no release.
 */
void fake_forge_release_everything(const char *index, const char *url);

/** Adds a repo holding FAKE_ROOT, with no parents, and `file` on its development branch. */
fake_repo *fake_forge_add(uint64_t number, const char *account, const char *name, const char *file);

/** How many calls the fake answered since the reset, and how many of them carried `token`. */
unsigned fake_forge_calls(void);
unsigned fake_forge_calls_with_token(const char *token);

/**
 * A format 2 repo.json naming `name`, whose import name is `plugin` so any repo name fits, with `homes` the JSON members
 * of its homes array; it lives until the reset.
 */
const char *fake_forge_file(const char *name, const char *homes);

#endif
