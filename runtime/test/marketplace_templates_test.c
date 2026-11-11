#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "cJSON.h"
#include "marketplace_check.h"
#include "spisor/file.h"

#define SKIP_STATUS 77
#define COMMIT_LENGTH 40
#define FETCH_PREFIX "fetch --depth 1 https://github.com/spisor/library "
#define TOKEN_HOST_OPTION "--token-host"
#define TOKEN_ENV_OPTION "--token-env"
#define INSTANCE_TOKEN_OPTIONS "--token-env LOOKUP_TOKEN --token-host \"${SERVER_URL#*://}\""
#define OPTIONAL_TOKEN_OPTIONS "--token-env MARKETPLACE_LOOKUP_TOKEN --token-host github.com"
#define BUILD_COMMAND "library/build/bin/forebay-host marketplace build --list marketplace.json --out marketplace.generated.json"

typedef struct template_host {
    const char *host;
    const char *ci_file;
    const char *publish_marker;
    const char *token_options;
    int minimum_entries;
} template_host;

static const template_host HOSTS[] = {
    {"github", ".github/workflows/marketplace.yml", "actions/deploy-pages", "--token-env GITHUB_TOKEN --token-host github.com", 1},
    {"gitlab", ".gitlab-ci.yml", "pages: true", OPTIONAL_TOKEN_OPTIONS, 1},
    {"bitbucket", "bitbucket-pipelines.yml", ".bitbucket.io", OPTIONAL_TOKEN_OPTIONS, 1},
    {"codeberg", ".forgejo/workflows/marketplace.yml", "git push --force origin pages", INSTANCE_TOKEN_OPTIONS, 1},
    {"forgejo", ".forgejo/workflows/marketplace.yml", "git push --force origin pages", INSTANCE_TOKEN_OPTIONS, 0},
    {"gitea", ".gitea/workflows/marketplace.yml", "git push --force origin pages", INSTANCE_TOKEN_OPTIONS, 0},
};
#define HOST_COUNT (sizeof HOSTS / sizeof HOSTS[0])

static char *read_text(const char *root, const char *host, const char *file) {
    char path[1024];
    snprintf(path, sizeof path, "%s/marketplace-template-%s/%s", root, host, file);
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(path, &length);
    if (bytes == NULL) {
        fprintf(stderr, "missing file: %s\n", path);
        check_failures++;
        return NULL;
    }
    char *text = calloc(length + 1, 1);
    if (text == NULL) {
        free(bytes);
        fprintf(stderr, "out of memory reading %s\n", path);
        check_failures++;
        return NULL;
    }
    memcpy(text, bytes, length);
    free(bytes);
    return text;
}

static int is_hex(char character) {
    return isdigit((unsigned char)character) || (character >= 'a' && character <= 'f');
}

static int pinned_commit_of(const char *text, char *commit) {
    const char *fetch = strstr(text, FETCH_PREFIX);
    if (fetch == NULL) {
        return 0;
    }
    const char *start = fetch + strlen(FETCH_PREFIX);
    size_t run = 0;
    while (is_hex(start[run])) {
        run++;
    }
    if (run != COMMIT_LENGTH) {
        return 0;
    }
    memcpy(commit, start, COMMIT_LENGTH);
    commit[COMMIT_LENGTH] = '\0';
    return 1;
}

static void checks_the_list(const char *root, const template_host *host) {
    char *text = read_text(root, host->host, "marketplace.json");
    if (text == NULL) {
        return;
    }
    cJSON *list = cJSON_Parse(text);
    char problem[256] = "not json";
    CHECK(list != NULL && runtime_marketplace_check_list(list, problem, sizeof problem));
    if (list == NULL || !runtime_marketplace_check_list(list, problem, sizeof problem)) {
        fprintf(stderr, "%s marketplace.json: %s\n", host->host, problem);
    }
    CHECK(list != NULL && cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(list, "entries")) >= host->minimum_entries);
    cJSON_Delete(list);
    free(text);
}

static int count_of(const char *text, const char *fragment) {
    int count = 0;
    for (const char *at = strstr(text, fragment); at != NULL; at = strstr(at + 1, fragment)) {
        count++;
    }
    return count;
}

static void checks_the_token_goes_to_one_named_host(const template_host *host, const char *text) {
    if (strstr(text, host->token_options) == NULL || count_of(text, TOKEN_HOST_OPTION) != 1 ||
        count_of(text, TOKEN_ENV_OPTION) != 1) {
        fprintf(stderr, "%s: %s must pass exactly \"%s\" and no other token option\n", host->host, host->ci_file,
                host->token_options);
        check_failures++;
    }
    if (strstr(text, "grep") != NULL) {
        fprintf(stderr, "%s: %s decides the token with grep\n", host->host, host->ci_file);
        check_failures++;
    }
}

static void checks_the_pipeline(const char *root, const template_host *host, char *shared_commit) {
    char *text = read_text(root, host->host, host->ci_file);
    if (text == NULL) {
        return;
    }
    char commit[COMMIT_LENGTH + 1];
    CHECK(pinned_commit_of(text, commit));
    if (pinned_commit_of(text, commit)) {
        if (shared_commit[0] == '\0') {
            memcpy(shared_commit, commit, sizeof commit);
        }
        check_text(commit, shared_commit, host->host);
    }
    CHECK(strstr(text, BUILD_COMMAND) != NULL);
    if (strstr(text, host->publish_marker) == NULL) {
        fprintf(stderr, "%s: %s does not publish (expected \"%s\")\n", host->host, host->ci_file, host->publish_marker);
        check_failures++;
    }
    free(text);
}

static int count_numbered_steps(const char *text) {
    int count = 0;
    for (const char *line = text; line != NULL; line = strchr(line, '\n') == NULL ? NULL : strchr(line, '\n') + 1) {
        if (line[0] == '1' + count && line[1] == '.' && line[2] == ' ') {
            count++;
        }
    }
    return count;
}

static void checks_the_content(const char *root, const template_host *host) {
    char *text = read_text(root, host->host, "CONTENT.md");
    if (text == NULL) {
        return;
    }
    int steps = count_numbered_steps(text);
    if (steps != 5) {
        fprintf(stderr, "%s CONTENT.md: %d numbered steps, expected 5\n", host->host, steps);
        check_failures++;
    }
    if (strstr(text, "The token goes only to ") == NULL || strstr(text, "without a token") == NULL ||
        strstr(text, "last copy") != NULL) {
        fprintf(stderr, "%s CONTENT.md: must name the token's host, say another host builds without a token, and not promise a last copy\n",
                host->host);
        check_failures++;
    }
    if (strstr(text, "Documentation: https://") == NULL) {
        fprintf(stderr, "%s CONTENT.md: no \"Documentation: https://\" line\n", host->host);
        check_failures++;
    }
    free(text);
}

int main(void) {
    const char *root = SPISOR_MARKETPLACE_TEMPLATES_PATH;
    if (root[0] == '\0') {
        printf("skipped: configure with -DSPISOR_MARKETPLACE_TEMPLATES_PATH=<folder holding marketplace-template-*>\n");
        return SKIP_STATUS;
    }
    char probe[1024];
    snprintf(probe, sizeof probe, "%s/marketplace-template-github/.git/HEAD", root);
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(probe, &length);
    if (bytes == NULL) {
        printf("skipped: no template checkouts under %s\n", root);
        return SKIP_STATUS;
    }
    free(bytes);
    char shared_commit[COMMIT_LENGTH + 1] = "";
    for (size_t index = 0; index < HOST_COUNT; index++) {
        checks_the_list(root, &HOSTS[index]);
        checks_the_pipeline(root, &HOSTS[index], shared_commit);
        checks_the_content(root, &HOSTS[index]);
    }
    return check_summary("marketplace_templates_test");
}
