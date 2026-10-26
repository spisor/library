#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"
#include "spisor/path.h"

#define MAX_SEGMENTS 16

typedef struct platform_rule {
    int windows;
    const char *name;
} platform_rule;

static const platform_rule platform_rules[] = {{0, "posix"}, {1, "win32"}};

static cJSON *load_vectors(void) {
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(SPISOR_VECTORS_PATH, &length);
    if (bytes == NULL) {
        fprintf(stderr, "cannot read %s\n", SPISOR_VECTORS_PATH);
        return NULL;
    }
    cJSON *vectors = cJSON_ParseWithLength((const char *)bytes, length);
    free(bytes);
    return vectors;
}

static size_t collect_segments(const cJSON *array, const char **segments) {
    size_t count = 0;
    const cJSON *segment = NULL;
    cJSON_ArrayForEach(segment, array) {
        if (count == MAX_SEGMENTS) {
            fprintf(stderr, "a vector has more than %d segments; raise MAX_SEGMENTS\n", MAX_SEGMENTS);
            check_failures++;
            return count;
        }
        segments[count++] = segment->valuestring;
    }
    return count;
}

static void check_join_vector(const cJSON *vector, const platform_rule *rule) {
    const char *segments[MAX_SEGMENTS];
    size_t count = collect_segments(cJSON_GetObjectItemCaseSensitive(vector, "segments"), segments);
    const char *expected = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(vector, rule->name));
    char *actual = spisor_path_join(rule->windows, segments, count);
    if (!check_text(actual, expected, rule->name)) {
        char *printed = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(vector, "segments"));
        fprintf(stderr, "  for segments %s\n", printed);
        free(printed);
    }
    free(actual);
}

static void check_absolute_vector(const cJSON *vector, const platform_rule *rule) {
    const char *path = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(vector, "path"));
    int expected = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(vector, rule->name));
    int actual = spisor_path_is_absolute(rule->windows, path) != 0;
    if (actual != expected) {
        fprintf(stderr, "%s isAbsolute(\"%s\"): expected %d, got %d\n", rule->name, path, expected, actual);
        check_failures++;
    }
}

static int matches_every_vector(void) {
    cJSON *vectors = load_vectors();
    CHECK(vectors != NULL);
    if (vectors == NULL) {
        return 0;
    }
    const cJSON *join_vectors = cJSON_GetObjectItemCaseSensitive(vectors, "join");
    const cJSON *absolute_vectors = cJSON_GetObjectItemCaseSensitive(vectors, "absolute");
    const cJSON *vector = NULL;
    for (size_t index = 0; index < sizeof(platform_rules) / sizeof(platform_rules[0]); index++) {
        cJSON_ArrayForEach(vector, join_vectors) {
            check_join_vector(vector, &platform_rules[index]);
        }
        cJSON_ArrayForEach(vector, absolute_vectors) {
            check_absolute_vector(vector, &platform_rules[index]);
        }
    }
    int total = cJSON_GetArraySize(join_vectors) + cJSON_GetArraySize(absolute_vectors);
    cJSON_Delete(vectors);
    return total;
}

static void expands(int windows, const char *path, const char *home, const char *expected) {
    char *actual = spisor_expand_home(windows, path, home);
    check_text(actual, expected, path);
    free(actual);
}

static void expands_home(void) {
    expands(0, "~", "/home/me", "/home/me");
    expands(0, "~/a", "/home/me", "/home/me/a");
    expands(0, "~\\a", "/home/me", "/home/me/a");
    expands(0, "a", "/home/me", "a");
    expands(0, "~a", "/home/me", "~a");
    expands(0, "~/", "/home/me/", "/home/me/");
    expands(1, "~", "C:\\Users\\me", "C:\\Users\\me");
    expands(1, "~/a", "C:\\Users\\me", "C:\\Users\\me\\a");
    expands(1, "~\\a/b", "C:\\Users\\me", "C:\\Users\\me\\a\\b");
    expands(1, "a", "C:\\Users\\me", "a");
}

int main(void) {
    int total = matches_every_vector();
    printf("path_test: %d vectors on each of 2 platform rules\n", total);
    CHECK(total > 0);
    expands_home();
    return check_summary("path_test");
}
