#ifndef RUNTIME_IDENTITY_RESOLVE_H
#define RUNTIME_IDENTITY_RESOLVE_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "export.h"

/**
 * identity.resolve: {id, lastKnown?, token?, apiBase?} answers {name, fullName, defaultBranch?} for the repo the id names,
 * or refuses with a fix. lastKnown is an "owner/name" a previous lookup saw. apiBase replaces the host's API root,
 * is not in library.json, and is refused unless the fetch test hook allows plain http, so only a test can aim a lookup.
 * token is an access token for the host the id names, sent as "Authorization: token <token>" to that host and to no
 * other, a redirect to another host included. Gitea, Forgejo and Codeberg answer 401 to a lookup by number without one.
 *
 * @implNote A repo is looked up by its number, which survives a rename; a number the host answers with is
 * always compared, as text, with the one asked for. Only when the host cannot answer by number (404, or 401 where
 * it wants a token for that route) is lastKnown asked, and its answer is accepted only when it carries the same
 * number, so a name someone else has since taken is refused. A local id has no host and answers from its own text.
 */
RUNTIME_EXPORT int32_t runtime_identity_resolve(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

typedef enum runtime_repo_file_status {
    RUNTIME_REPO_FILE_READ,
    RUNTIME_REPO_FILE_ABSENT,
    RUNTIME_REPO_FILE_TOO_LARGE,
    RUNTIME_REPO_FILE_UNAVAILABLE,
    RUNTIME_REPO_FILE_OUT_OF_MEMORY
} runtime_repo_file_status;

/**
 * Reads the file at path, from the root of the repo id names, at that repo's default branch, anonymously and within the
 * same body limit as a lookup. READ fills *content, freed with free, and *length; ABSENT is the host saying the
 * file is not there; TOO_LARGE is a body above the limit. UNAVAILABLE is anything else, a repo that cannot be
 * resolved included, and then *refusal is set to the resolve refusal when that was the cause. apiBase aims the read as it aims
 * identity.resolve.
 *
 * @implNote GitHub's file is read from raw.githubusercontent.com at HEAD, which is the default branch and is not
 * counted against the API's anonymous rate limit; a Gitea or Forgejo host answers the file raw from its API root, and
 * the default branch when no ref is given. Both measured 2026-11-10 to answer 200 anonymously for a nested path and
 * 404 for a missing file. The owner/name comes from the lookup's own answer, so a renamed repo is read where it is now.
 * A file is fetched by name, so the repo is looked up by number again afterwards and the file is kept only when that
 * answers the same owner/name: a name taken between the two would otherwise serve another repo's file.
 */
runtime_repo_file_status runtime_identity_read_file(const char *id, const char *api_base, const char *path,
                                                    uint8_t **content, size_t *length, cJSON **refusal);

/**
 * Writes into url, of size bytes, the address of route below repos/<full_name> on the host id names, such as
 * "releases/latest", under apiBase when it is given. 0 when the id has no built-in resolver, full_name is not an
 * owner/name, or the address does not fit.
 */
int runtime_identity_repo_url(const char *id, const char *api_base, const char *full_name, const char *route, char *url,
                              size_t size);

#endif
