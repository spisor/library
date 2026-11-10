#include "catalog_schema.h"

#define ITEM "/definitions/item"
#define VERSION "/definitions/version"
#define FILE_REFERENCE "/definitions/fileRef"
#define APPLICATION "/definitions/application"
#define HOME APPLICATION "/properties/home"
#define PACKAGE APPLICATION "/properties/packages/additionalProperties/anyOf/1"
#define VERSION_PATTERN                                                                                          \
    "^(?![\\s\\S]*\\.\\.)[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z.-]*[0-9A-Za-z-])?(\\+[0-9A-Za-z.-]*[0-9A-Za-z-])?(?!" \
    "[\\s\\S])"
#define NAME_PATTERN "^[A-Za-z0-9][A-Za-z0-9._-]{0,63}(?![\\s\\S])"

const runtime_catalog_schema_text runtime_catalog_schema_patterns[PATTERN_COUNT] = {
    [PATTERN_ITEM_ID] = {"/properties/items/propertyNames/pattern",
                         "^(?!(?:con|prn|aux|nul|com[1-9]|lpt[1-9])(?:[.]|(?![\\s\\S])))(?:[a-z0-9]+([-.][a-z0-9]+)*|(?:local:[^/\\s]+|[a-z][a-z0-9-]*(?:@[a-z0-9.-]+)?:[0-9]+)(?:/(?!\\.{1,2}(?:/|(?![\\s\\S])))[a-z0-9._-]+)*)(?![\\s\\S])"},
    [PATTERN_VERSION_KEY] = {ITEM "/properties/versions/propertyNames/pattern", VERSION_PATTERN},
    [PATTERN_LATEST] = {ITEM "/properties/latest/pattern", VERSION_PATTERN},
    [PATTERN_FILE_KEY] = {"/definitions/files/propertyNames/pattern",
                          "^[^/\\\\:]*[^/\\\\:. ](?:/[^/\\\\:]*[^/\\\\:. ])*(?![\\s\\S])"},
    [PATTERN_SHA256] = {FILE_REFERENCE "/properties/sha256/pattern", "^[0-9a-f]{64}(?![\\s\\S])"},
    [PATTERN_BINARY] = {APPLICATION "/properties/binary/pattern", NAME_PATTERN},
    [PATTERN_XDG_SUBDIR] = {HOME "/properties/xdgSubdir/pattern", NAME_PATTERN},
    [PATTERN_CANDIDATE] = {HOME "/properties/candidates/items/pattern", "^(?:~/|/|[A-Za-z]:[\\/])"},
    [PATTERN_PACKAGE_NAME] = {APPLICATION "/properties/packages/propertyNames/pattern", NAME_PATTERN},
};

static const char *const ROOT_REQUIRED[] = {"format", "items", NULL};
static const char *const ITEM_REQUIRED[] = {"kind", "versions", "latest", NULL};
static const char *const FILES[] = {"files", NULL};
static const char *const PLATFORMS[] = {"platforms", NULL};
static const char *const FILE_REFERENCE_REQUIRED[] = {"url", "sha256", "size", NULL};
static const char *const APPLICATION_REQUIRED[] = {"id", "label", "binary", "home", "packages", NULL};
static const char *const HOME_REQUIRED[] = {"candidates", NULL};
static const char *const PACKAGE_REQUIRED[] = {"name", NULL};
static const char *const KIND[] = {"kind", NULL};
static const char *const APPLICATION_MEMBER[] = {"application", NULL};
static const char *const RUNTIME_MEMBER[] = {"runtime", NULL};
static const char *const KINDS[] = {"plugin", "host", "library", NULL};
static const char *const RUNTIMES[] = {"javascript", "jvm", "native", NULL};
static const char *const PLUGIN_OR_LIBRARY[] = {"plugin", "library", NULL};

const runtime_catalog_schema_list runtime_catalog_schema_lists[LIST_COUNT] = {
    [REQUIRED_ROOT] = {"/required", ROOT_REQUIRED},
    [REQUIRED_ITEM] = {ITEM "/required", ITEM_REQUIRED},
    [REQUIRED_VERSION_FILES] = {VERSION "/anyOf/0/required", FILES},
    [REQUIRED_VERSION_PLATFORMS] = {VERSION "/anyOf/1/required", PLATFORMS},
    [REQUIRED_PLATFORM] = {VERSION "/properties/platforms/additionalProperties/required", FILES},
    [REQUIRED_FILE_REFERENCE] = {FILE_REFERENCE "/required", FILE_REFERENCE_REQUIRED},
    [REQUIRED_APPLICATION] = {APPLICATION "/required", APPLICATION_REQUIRED},
    [REQUIRED_HOME] = {HOME "/required", HOME_REQUIRED},
    [REQUIRED_PACKAGE] = {PACKAGE "/required", PACKAGE_REQUIRED},
    [REQUIRED_APPLICATION_RULE_IF] = {ITEM "/allOf/0/if/required", KIND},
    [REQUIRED_APPLICATION_RULE_NOT] = {ITEM "/allOf/0/then/not/required", APPLICATION_MEMBER},
    [REQUIRED_RUNTIME_RULE_IF] = {ITEM "/allOf/1/if/required", KIND},
    [REQUIRED_RUNTIME_RULE_THEN] = {ITEM "/allOf/1/then/required", RUNTIME_MEMBER},
    [ENUM_KIND] = {ITEM "/properties/kind/enum", KINDS},
    [ENUM_RUNTIME] = {ITEM "/properties/runtime/enum", RUNTIMES},
    [ENUM_APPLICATION_RULE_KINDS] = {ITEM "/allOf/0/if/properties/kind/enum", PLUGIN_OR_LIBRARY},
};

const runtime_catalog_schema_bound runtime_catalog_schema_bounds[BOUND_COUNT] = {
    [MIN_LENGTH_URL] = {FILE_REFERENCE "/properties/url/minLength", 1},
    [MIN_LENGTH_INSTALL_METHOD] = {ITEM "/properties/installMethod/minLength", 1},
    [MIN_LENGTH_APPLICATION_ID] = {APPLICATION "/properties/id/minLength", 1},
    [MIN_LENGTH_APPLICATION_LABEL] = {APPLICATION "/properties/label/minLength", 1},
    [MIN_LENGTH_ENV_OVERRIDE] = {HOME "/properties/envOverride/minLength", 1},
    [MIN_LENGTH_NATIVE_ENV] = {HOME "/properties/nativeEnv/minLength", 1},
    [MINIMUM_SIZE] = {FILE_REFERENCE "/properties/size/minimum", 0},
    [MIN_ITEMS_CANDIDATES] = {HOME "/properties/candidates/minItems", 1},
};

const runtime_catalog_schema_text runtime_catalog_schema_constants[CONSTANT_COUNT] = {
    [CONSTANT_FORMAT] = {"/properties/format/const", "1"},
    [CONSTANT_RUNTIME_RULE_KIND] = {ITEM "/allOf/1/if/properties/kind/const", "\"host\""},
};
