#ifndef RUNTIME_PUBLIC_UI_H
#define RUNTIME_PUBLIC_UI_H

/**
 * @file
 * runtime's `ui` service: the navigation every host lists, the screens a plugin's views and settings open into, the
 * calls a screen's bindings need answered first, and the host's own Plugins screen. A size-prefixed struct is read only
 * up to its `size`. Text the caller hands in is NUL-terminated UTF-8 borrowed for the call; what an answer or a
 * refusal points at is the provider's until the release for its kind. A plugin's manifest, the user's settings, a
 * binding's arguments, an answer and a node's data and props are value trees, because their shape is the plugin's or
 * the user's; every shape this service decides is a struct.
 */

#include <stddef.h>
#include <stdint.h>

#include "runtime/definition.h"
#include "spisor/status.h"
#include "spisor/value.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What a navigation entry or a screen is. Append-only: a published value keeps its number. */
typedef enum runtime_ui_kind {
    RUNTIME_UI_KIND_VIEW = 1,
    RUNTIME_UI_KIND_SETTINGS = 2,
    RUNTIME_UI_KIND_ACTION = 3,
    RUNTIME_UI_KIND_HOST = 4
} runtime_ui_kind;

/**
 * The plugin manifests, a list of records each with its id as text, and the user's `ui` settings, a record. First
 * shape ends at `ui`.
 */
typedef struct runtime_ui_navigation_argument {
    uint32_t size;
    const spisor_value *descriptions;
    const spisor_value *ui;
} runtime_ui_navigation_argument;

/**
 * One navigation entry: the `plugin` it belongs to, its `kind`, the `id` of its view, settings or action, its
 * `title`, NULL for none, and whether the user `pinned` it, 0 or 1. An element of an array, so it has no size.
 */
typedef struct runtime_ui_entry {
    const char *plugin;
    /** A runtime_ui_kind. */
    uint32_t kind;
    const char *id;
    const char *title;
    uint32_t pinned;
} runtime_ui_entry;

/**
 * The entries in the order every host lists them, and the `preferences`, a record of everything under the user's ui
 * settings but its plugins. First shape ends at `storage`.
 */
typedef struct runtime_ui_navigation_answer {
    uint32_t size;
    const runtime_ui_entry *entries;
    size_t entry_count;
    const spisor_value *preferences;
    void *storage;
} runtime_ui_navigation_answer;

/** One plugin manifest, a record with its id as text. First shape ends at `description`. */
typedef struct runtime_ui_settings_view_argument {
    uint32_t size;
    const spisor_value *description;
} runtime_ui_settings_view_argument;

/** A node of a settings view. Append-only: a published value keeps its number. */
typedef enum runtime_ui_settings_component {
    RUNTIME_UI_COMPONENT_FORM = 1,
    RUNTIME_UI_COMPONENT_FIELD = 2
} runtime_ui_settings_component;

/**
 * One node of a settings view: its `id`, its `parent`'s id, NULL for the form itself, its `component`, a
 * runtime_ui_settings_component, and for a field the setting's key as `field` and its `label`, each NULL for none. An
 * element of an array, so it has no size.
 */
typedef struct runtime_ui_settings_node {
    const char *id;
    const char *parent;
    uint32_t component;
    const char *field;
    const char *label;
} runtime_ui_settings_node;

/**
 * The view that edits a plugin's settings: its `title` and its nodes, the form first. First shape ends at `storage`.
 */
typedef struct runtime_ui_settings_view_answer {
    uint32_t size;
    const char *title;
    const runtime_ui_settings_node *nodes;
    size_t node_count;
    void *storage;
} runtime_ui_settings_view_answer;

/**
 * One call a binding makes: the `plugin` whose view binds it, the `service` and `method` it calls, each text that may
 * be empty, and its `arguments`, a record. Two calls are the same exactly when all four are. Embedded in other
 * structs, so it has no size.
 */
typedef struct runtime_ui_target {
    const char *plugin;
    const char *service;
    const char *method;
    const spisor_value *arguments;
} runtime_ui_target;

/**
 * One ui-data check an answer needs: the component it is checked `against`, and when `has_members` is 1 the only
 * `members` of an object answer it reads, which may be none; when `has_members` is 0 it reads the whole answer and
 * `members` is not read. Embedded in other structs, so it has no size.
 */
typedef struct runtime_ui_check {
    const char *against;
    uint32_t has_members;
    const char *const *members;
    size_t member_count;
} runtime_ui_check;

/** One call a screen still needs answered, with every check its answer needs. An element of an array. */
typedef struct runtime_ui_call {
    runtime_ui_target target;
    const runtime_ui_check *checks;
    size_t check_count;
} runtime_ui_call;

/** One issue a check found: its `message`, its `fix` and the `path` at fault, NULL for none. An element of an array. */
typedef struct runtime_ui_issue {
    const char *message;
    const char *fix;
    const char *path;
} runtime_ui_issue;

/** A check as plan named it and the issues the engine found against it, none for an answer that passes. */
typedef struct runtime_ui_checked {
    runtime_ui_check check;
    const runtime_ui_issue *issues;
    size_t issue_count;
} runtime_ui_checked;

/**
 * Why a node or a failed reply is in error: its `detail`, its `fix` and the `path` at fault, NULL for none. A node
 * points to one and a reply embeds one, so it has no size.
 */
typedef struct runtime_ui_error {
    const char *detail;
    const char *fix;
    const char *path;
} runtime_ui_error;

/** How a call plan asked for ended. Append-only: a published value keeps its number. */
typedef enum runtime_ui_outcome {
    RUNTIME_UI_OUTCOME_ANSWERED = 1,
    RUNTIME_UI_OUTCOME_FAILED = 2,
    RUNTIME_UI_OUTCOME_UNAVAILABLE = 3
} runtime_ui_outcome;

/**
 * One call plan asked for and its `outcome`, a runtime_ui_outcome: ANSWERED with its `answer`, a value of any kind but
 * none, and what each of its checks found as `checked`; FAILED with its `error`; UNAVAILABLE with neither. The fields
 * of the other outcomes are not read. An element of an array, so it has no size.
 */
typedef struct runtime_ui_reply {
    runtime_ui_target target;
    uint32_t outcome;
    const spisor_value *answer;
    const runtime_ui_checked *checked;
    size_t checked_count;
    runtime_ui_error error;
} runtime_ui_reply;

/**
 * The plugin manifests as navigation takes them, the `screen` to open, a navigation entry of which only its plugin,
 * kind and id are read, the id NULL for none, the `replies` so far, none for a first plan, and the `round`, how many
 * plans of this screen came before this one, 0 for a first plan and for an argument that ends before it; render does
 * not read it. First shape ends at `reply_count`.
 */
typedef struct runtime_ui_screen_argument {
    uint32_t size;
    const spisor_value *descriptions;
    const runtime_ui_entry *screen;
    const runtime_ui_reply *replies;
    size_t reply_count;
    uint32_t round;
} runtime_ui_screen_argument;

/**
 * Every call the screen still needs answered, each listed once, none once the replies hold them all. First shape ends
 * at `storage`.
 */
typedef struct runtime_ui_plan_answer {
    uint32_t size;
    const runtime_ui_call *calls;
    size_t call_count;
    void *storage;
} runtime_ui_plan_answer;

/** How a rendered node stands. Append-only: a published value keeps its number. */
typedef enum runtime_ui_state {
    RUNTIME_UI_STATE_READY = 1,
    RUNTIME_UI_STATE_ERROR = 2,
    RUNTIME_UI_STATE_UNAVAILABLE = 3
} runtime_ui_state;

/**
 * One rendered node: its `id`, its `component`, its `parent`'s id, NULL for a root, its `state`, a runtime_ui_state,
 * the `data` a ready bound node carries, NULL for none, its `error`, NULL unless its state is ERROR, and its `props`, a
 * record of the presentation members it declares. An element of an array, so it has no size.
 */
typedef struct runtime_ui_node {
    const char *id;
    const char *component;
    const char *parent;
    uint32_t state;
    const spisor_value *data;
    const runtime_ui_error *error;
    const spisor_value *props;
} runtime_ui_node;

/**
 * A headless screen: the `plugin` it shows, NULL for the host's own, its `kind`, a runtime_ui_kind, the `view` it
 * opened, NULL for none, its `title`, NULL for none, and its nodes in the order the view declares them. First shape
 * ends at `storage`.
 */
typedef struct runtime_ui_screen {
    uint32_t size;
    const char *plugin;
    uint32_t kind;
    const char *view;
    const char *title;
    const runtime_ui_node *nodes;
    size_t node_count;
    void *storage;
} runtime_ui_screen;

/**
 * The three groups lists' plugins takes, each a list of records with its id as text. First shape ends at `available`.
 */
typedef struct runtime_ui_plugin_list_argument {
    uint32_t size;
    const spisor_value *installed;
    const spisor_value *detected;
    const spisor_value *available;
} runtime_ui_plugin_list_argument;

/**
 * One row of the host's Plugins screen, each fact the library decides a field: the plugin's `id`, its `state`,
 * `unavailable` 1 for a row that cannot be used and `install` 1 for one that can be installed, and the caller's `row`
 * as it was first given, a record. An element of an array, so it has no size.
 */
typedef struct runtime_ui_plugin_row {
    const char *id;
    /** A runtime_list_state. */
    uint32_t state;
    uint32_t unavailable;
    uint32_t install;
    const spisor_value *row;
} runtime_ui_plugin_row;

/**
 * The host's own Plugins screen: its `title` and its rows, installed, then detected, then available. First shape ends
 * at `storage`.
 */
typedef struct runtime_ui_plugin_list_answer {
    uint32_t size;
    const char *title;
    const runtime_ui_plugin_row *rows;
    size_t row_count;
    void *storage;
} runtime_ui_plugin_list_answer;

/**
 * One action to run: the `description` of the plugin that declares it, a manifest as settings_view takes it, the
 * `action`'s id, the `input` laid over its declared argument, a record, NULL for none, and the row it runs on: the
 * row's `item`, NULL for none, and when `has_items` is 1 its `items`, which may be none; when `has_items` is 0 `items`
 * is not read. First shape ends at `item_count`.
 */
typedef struct runtime_ui_action_argument {
    uint32_t size;
    const spisor_value *description;
    const char *action;
    const spisor_value *input;
    const char *item;
    uint32_t has_items;
    const char *const *items;
    size_t item_count;
} runtime_ui_action_argument;

/** The call the action makes, its arguments merged as action merges them. First shape ends at `storage`. */
typedef struct runtime_ui_action_answer {
    uint32_t size;
    runtime_ui_target target;
    void *storage;
} runtime_ui_action_answer;

/**
 * The ui service. It reads no file and writes nothing. Every operation answers MALFORMED for an argument short of its
 * first shape, an answer that is not empty, a manifest list that is not a list of records each with its id as text, a
 * value that is not well-formed or not of the kind its field names, a count whose array is NULL, a required text that
 * is NULL, or a kind, outcome or flag outside its values; and OUT_OF_MEMORY, which never reads as a refusal. The answer
 * arguments are required and the refusal may be NULL for none. First shape ends at `release_refusal`.
 */
typedef struct runtime_ui_definition {
    uint32_t size;
    void *context;
    /**
     * Every plugin's views, its settings screen and its actions merged with the customisation under the ui settings'
     * plugins, so every host lists the same entries in the same order.
     *
     * @return OK; MALFORMED; OUT_OF_MEMORY.
     * @implNote Plugins without an integer order sort after every ordered one, then by id. A customisation naming no
     * plugin, a pinned or default view the plugin does not declare and a value of the wrong type are ignored, because
     * settings follow the user to hosts that do not run every plugin and one bad edit must never hide a plugin.
     */
    spisor_status (*navigation)(void *context, const runtime_ui_navigation_argument *argument,
                                runtime_ui_navigation_answer *answer, spisor_refusal *refusal);
    /**
     * The view that edits a plugin's settings, one form holding one field per setting in key order.
     *
     * @return OK; MALFORMED; OUT_OF_MEMORY.
     */
    spisor_status (*settings_view)(void *context, const runtime_ui_settings_view_argument *argument,
                                   runtime_ui_settings_view_answer *answer, spisor_refusal *refusal);
    /**
     * Every call the screen's bindings still need answered, given the replies so far.
     *
     * @return OK; MALFORMED; REFUSED for a screen of a plugin no manifest describes, a view it does not declare, an
     * action, or a screen of no view or settings, and for a `round` of 16 or more, since a screen still naming calls
     * after 16 rounds is never rendered; OUT_OF_MEMORY.
     * @implNote C may not call back, so the caller answers each call through its own engine, checks the answer against
     * every check (the engine's job, never C's) and plans again with the replies so far and the round counted up. A
     * binding whose arguments depend on another's answer takes one more round, and no screen nests deeper than 16.
     */
    spisor_status (*plan)(void *context, const runtime_ui_screen_argument *argument, runtime_ui_plan_answer *answer,
                          spisor_refusal *refusal);
    /**
     * The headless screen, once plan has no call left.
     *
     * @return OK; MALFORMED; REFUSED as plan refuses, and for a bound node whose call has no reply or whose answer
     * lacks a check plan named; OUT_OF_MEMORY.
     * @implNote A failed, late or misshapen answer puts only its own node in error; the rest render. A chart node's
     * own chart picks which members of its answer it carries, never the answer.
     */
    spisor_status (*render)(void *context, const runtime_ui_screen_argument *argument, runtime_ui_screen *answer,
                            spisor_refusal *refusal);
    /**
     * The host's own Plugins screen: the three groups as one list, each row that cannot be used marked unavailable
     * and installable.
     *
     * @return OK; MALFORMED; OUT_OF_MEMORY.
     * @implNote Each fact the library decides is a field and the caller's row comes back untouched, so a host keeps
     * every member it put in a row; a plugin view cannot bind a host list, which is why this is a host screen.
     */
    spisor_status (*plugin_list)(void *context, const runtime_ui_plugin_list_argument *argument,
                                 runtime_ui_plugin_list_answer *answer, spisor_refusal *refusal);
    void (*release_navigation_answer)(void *context, runtime_ui_navigation_answer *answer);
    void (*release_settings_view_answer)(void *context, runtime_ui_settings_view_answer *answer);
    void (*release_plan_answer)(void *context, runtime_ui_plan_answer *answer);
    void (*release_screen)(void *context, runtime_ui_screen *answer);
    void (*release_plugin_list_answer)(void *context, runtime_ui_plugin_list_answer *answer);
    void (*release_refusal)(void *context, spisor_refusal *refusal);
    /**
     * The call one of a plugin's declared actions makes: the plugin, the service and method its `call` names, and as
     * arguments its declared `argument`, then the input laid over it, then the row's item and items laid over both,
     * each later one replacing a member of the same name.
     *
     * @return OK; MALFORMED; REFUSED for an action the manifest does not declare under its `actions`, and for one whose
     * `call` names no service or method as text; OUT_OF_MEMORY.
     * @implNote A row may set only `item` and `items`, and no input overrides them, so a row always runs on the row it
     * was clicked on. A declared argument that is not a record reads as none, as a bound node's does.
     */
    spisor_status (*action)(void *context, const runtime_ui_action_argument *argument,
                            runtime_ui_action_answer *answer, spisor_refusal *refusal);
    void (*release_action_answer)(void *context, runtime_ui_action_answer *answer);
} runtime_ui_definition;

/** The static runtime_ui_definition, living until the library is unloaded. */
RUNTIME_DEFINITION_API const void *runtime_ui_definition_of(void);

#ifdef __cplusplus
}
#endif

#endif
