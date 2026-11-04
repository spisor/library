#ifndef RUNTIME_UI_REFUSAL_H
#define RUNTIME_UI_REFUSAL_H

#include "cJSON.h"

/** The {message, fix} refusal whose message is the texts after fix joined, up to a NULL; NULL when memory runs out. */
cJSON *runtime_ui_refusal(const char *fix, ...);

#endif
