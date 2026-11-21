#ifndef RUNTIME_PROCESS_ENVIRONMENT_H
#define RUNTIME_PROCESS_ENVIRONMENT_H

#include "cJSON.h"

/** This process's environment as an object of strings, every name once; NULL when memory runs out. */
cJSON *runtime_process_environment(void);

#endif
