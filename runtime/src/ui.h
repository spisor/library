#ifndef RUNTIME_UI_H
#define RUNTIME_UI_H

#include "cJSON.h"

/**
 * The ui service's functions, which its definition calls: each answers its result, or NULL with *refusal set to
 * {message, fix}, or with it NULL when memory runs out.
 *
 * ui's navigation: over the plugin manifests and the user's `ui` settings, answering
 * { entries: [{ plugin, kind, id, title?, pinned }], preferences }; NULL only when memory runs out.
 */
cJSON *runtime_ui_navigation(const cJSON *descriptions, const cJSON *ui);

/** ui's settings_view: over one plugin manifest, answering { title, nodes }, one form of fields. */
cJSON *runtime_ui_settings_view(const cJSON *description, cJSON **refusal);

/**
 * ui's plan: over the descriptions, the navigation entry to open as `entry` and the answers so far, which may be
 * NULL, answering { calls: [{ plugin, binding: { service, method }, arguments, checks: [{ against, members? }] }] }
 * for every binding the screen still needs answered, none once answers holds them all.
 */
cJSON *runtime_ui_plan(const cJSON *descriptions, const cJSON *entry, const cJSON *answers, cJSON **refusal);

/**
 * ui's render: as plan takes them, answering the headless screen { plugin, kind, view?, title?,
 * nodes: [{ id, component, parent?, state, data?, error?, props }] }. An answer is one call of plan with its outcome:
 * { answer, checked: [{ against, members?, issues }] }, { error: { detail, fix } } or { unavailable: true }.
 */
cJSON *runtime_ui_render(const cJSON *descriptions, const cJSON *entry, const cJSON *answers, cJSON **refusal);

/**
 * ui's plugin_list: installed, detected and available, as lists' plugins takes them, answering the host's own
 * Plugins screen { kind: "host", title, nodes }: one ready table of the three-state list.
 */
cJSON *runtime_ui_plugin_list(const cJSON *installed, const cJSON *detected, const cJSON *available,
                              cJSON **refusal);

#endif
