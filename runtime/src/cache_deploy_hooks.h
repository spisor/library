#ifndef RUNTIME_CACHE_DEPLOY_HOOKS_H
#define RUNTIME_CACHE_DEPLOY_HOOKS_H

/**
 * Stops every later deploy after its first `moves` moves into pluginDir, as a crash would, leaving its staging folder
 * named for a process that is not running; a negative count lets every deploy finish.
 *
 * @implNote Defined only when SPISOR_CACHE_TEST_HOOKS is set, which only the test's own build of the cache sets.
 */
void runtime_cache_stop_deploys_after(long moves);

#endif
