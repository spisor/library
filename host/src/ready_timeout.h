#ifndef HOST_READY_TIMEOUT_H
#define HOST_READY_TIMEOUT_H

/**
 * How long a launch waits for its session host to publish that it is ready.
 *
 * @implNote Ten seconds, except in the test build of this file, which takes SPISOR_TEST_READY_TIMEOUT
 * milliseconds from the environment; no argument or setting reaches it, so nothing a user passes shortens it.
 */
unsigned host_ready_timeout_milliseconds(void);

#endif
