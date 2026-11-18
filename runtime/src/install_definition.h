#ifndef RUNTIME_INSTALL_DEFINITION_H
#define RUNTIME_INSTALL_DEFINITION_H

#include "cJSON.h"
#include "runtime/install.h"
#include "spisor/status.h"

/** Fills a runtime_install_register_loader_answer from what hosts' register answered, as a runtime_answer_filler. */
spisor_status runtime_fill_register_answer(const cJSON *answered, void *filled);

#endif
