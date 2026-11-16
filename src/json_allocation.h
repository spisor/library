#ifndef SPISOR_JSON_ALLOCATION_H
#define SPISOR_JSON_ALLOCATION_H

/**
 * Makes cJSON allocate through spisor_memory_allocate and spisor_memory_release, so the binary's one allocator serves
 * every cJSON tree and printed text, and a test's counting allocator fails cJSON's allocations too. A binary calls it
 * where it initialises, before any cJSON allocation, as runtime does when a host first takes a definition.
 *
 * @implNote It rewrites cJSON's process-wide hooks with the same two functions every time, so calling it again is
 * harmless; a text cJSON printed is freed with cJSON_free from then on.
 */
void spisor_json_use_library_allocator(void);

#endif
