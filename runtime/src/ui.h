#ifndef RUNTIME_UI_H
#define RUNTIME_UI_H

#include "cJSON.h"

/**
 * The ui service's operations over JSON, which its definition serves: each answers its result, or NULL with
 * *refusal set to {message, fix}, or with it NULL when memory runs out.
 *
 * ui's navigation: { descriptions, ui }, the plugin manifests and the user's `ui` settings, answering
 * { entries: [{ plugin, kind, id, title?, pinned }], preferences }.
 */
cJSON *runtime_ui_navigation_operation(const cJSON *argument, cJSON **refusal);

/** ui's settings_view: { description }, one plugin manifest, answering { title, nodes }, one form of fields. */
cJSON *runtime_ui_settings_view_operation(const cJSON *argument, cJSON **refusal);

/**
 * ui's plan: { descriptions, screen, answers? }, where screen is the navigation entry to open, answering
 * { calls: [{ plugin, binding: { service, method }, arguments, checks: [{ against, members? }] }] } for every
 * binding the screen still needs answered, none once answers holds them all.
 */
cJSON *runtime_ui_plan_operation(const cJSON *argument, cJSON **refusal);

/**
 * ui's render: { descriptions, screen, answers }, answering the headless screen { plugin, kind, view?, title?,
 * nodes: [{ id, component, parent?, state, data?, error?, props }] }. An answer is one call of plan with its outcome:
 * { answer, checked: [{ against, members?, issues }] }, { error: { detail, fix } } or { unavailable: true }.
 */
cJSON *runtime_ui_render_operation(const cJSON *argument, cJSON **refusal);

/**
 * ui's plugin_list: { installed, detected, available }, as lists' plugins takes them, answering the host's own
 * Plugins screen { kind: "host", title, nodes }: one ready table of the three-state list.
 */
cJSON *runtime_ui_plugin_list_operation(const cJSON *argument, cJSON **refusal);

#endif
