#ifndef SPISOR_JSON_ALLOCATION_H
#define SPISOR_JSON_ALLOCATION_H

/**
 * Makes cJSON allocate through spisor_memory_allocate and spisor_memory_release, so the binary's one allocator serves
 * every cJSON tree and printed text, and a test's counting allocator fails cJSON's allocations too. A text cJSON
 * printed is freed with cJSON_free from then on.
 *
 * @implNote Called exactly once per binary, before any thread can reach its cJSON: cJSON_InitHooks resets the hooks to
 * malloc and free before it installs these, so a call while another thread uses cJSON can pair an allocation with the
 * other allocator's free. The runtime library calls it as it loads; a binary linking the objects statically calls it
 * on the first line of its main.
 */
void spisor_json_use_library_allocator(void);

#endif
