#ifndef RUNTIME_UI_H
#define RUNTIME_UI_H

#include <stddef.h>
#include <stdint.h>

#include "export.h"

/**
 * ui.navigation: { descriptions, ui }, the plugin manifests and the user's `ui` settings, answering
 * { entries: [{ plugin, kind, id, title, pinned }], preferences }: every plugin's views, its settings
 * screen and its actions merged with the customisation under ui.plugins, so every host lists the same
 * entries in the same order.
 *
 * @implNote Plugins without an integer `order` sort after every ordered one, then by id. An id under
 * ui.plugins that names no plugin, a pinned or default view the plugin does not declare and a value of
 * the wrong type are ignored silently, because settings follow the user to hosts that do not run every
 * plugin and one bad edit must never hide a plugin.
 */
RUNTIME_EXPORT int32_t runtime_ui_navigation(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/**
 * ui.settingsView: { description }, one plugin manifest, answering the view that edits its settings:
 * { title: "Settings", nodes }, one `form` node holding one `field` node per setting in key order.
 */
RUNTIME_EXPORT int32_t runtime_ui_settings_view(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/**
 * ui.plan: { descriptions, screen, answers? }, where screen is the navigation entry to open, answering
 * { calls: [{ plugin, binding: { service, method }, arguments, checks }] } for every binding the screen
 * still needs answered, or { done: true } once answers holds them all. Each call is listed once however
 * many nodes bind it, and checks names every ui-data check its answer needs: { against, members? }, where
 * members, when present, are the only members of an object answer the check reads.
 *
 * @implNote C may not call back, so the layer calls each binding through its own engine, checks the
 * answer against every check (the engine's job, never C's) and asks again with the answers so far.
 */
RUNTIME_EXPORT int32_t runtime_ui_plan(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/**
 * ui.render: { descriptions, screen, answers }, answering the headless screen: { plugin, kind, view?,
 * title, nodes: [{ id, component, parent?, state, data?, error?, props }] }. An answer is one call of
 * ui.plan with its outcome: { answer, checked: [{ against, members?, issues }] }, { error: { detail, fix } }
 * or { unavailable: true }.
 *
 * @implNote A failed, late or misshapen answer puts only its own node in `error`; the rest render. A
 * chart node's own `chart` picks which members of its answer it carries, never the answer.
 */
RUNTIME_EXPORT int32_t runtime_ui_render(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/**
 * ui.pluginList: { installed, detected, available }, as lists.plugins takes them, answering the host's
 * own Plugins screen { kind: "host", title, nodes }: one ready table of the three-state list, each row
 * that cannot be used marked unavailable and installable.
 *
 * @implNote A plugin view cannot bind a host list, which is why this is a host screen with no plugin.
 */
RUNTIME_EXPORT int32_t runtime_ui_plugin_list(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
