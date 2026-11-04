#ifndef RUNTIME_HOSTS_REQUEST_H
#define RUNTIME_HOSTS_REQUEST_H

#include "cJSON.h"

/**
 * A new argument for another service, carrying a copy of argument's places when it has one, so that
 * service refuses a missing or unusable one in its own words. NULL when memory runs out.
 */
cJSON *runtime_hosts_places_request(const cJSON *argument);

#endif
