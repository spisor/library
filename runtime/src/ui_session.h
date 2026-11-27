#ifndef RUNTIME_UI_SESSION_H
#define RUNTIME_UI_SESSION_H

#include "cJSON.h"

/**
 * The session of host_id under places, {state, cache, portable, ...}, as ui's session answers it: {host, home, places,
 * plugins: [{id, version}], descriptions, ui, installed, detected, available, problems, sources}, places as given,
 * writing nothing. NULL with *refusal set to {message, fix} only when the host's layers cannot be read, or with it NULL
 * when memory runs out.
 */
cJSON *runtime_ui_session(const cJSON *places, const char *host_id, cJSON **refusal);

#endif
