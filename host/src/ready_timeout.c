#include "ready_timeout.h"

#if defined(HOST_READY_TIMEOUT_TEST_HOOK)
#include <stdlib.h>
#endif

#define READY_TIMEOUT_MILLISECONDS 10000u

unsigned host_ready_timeout_milliseconds(void) {
#if defined(HOST_READY_TIMEOUT_TEST_HOOK)
    const char *overridden = getenv("SPISOR_TEST_READY_TIMEOUT");
    unsigned long milliseconds = overridden == NULL ? 0 : strtoul(overridden, NULL, 10);
    return milliseconds == 0 ? READY_TIMEOUT_MILLISECONDS : (unsigned)milliseconds;
#else
    return READY_TIMEOUT_MILLISECONDS;
#endif
}
