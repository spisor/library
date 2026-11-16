#ifndef SPISOR_FILE_READING_H
#define SPISOR_FILE_READING_H

#include <stddef.h>
#include <stdint.h>

/**
 * @file
 * The steps of reading a JSON file, allocating through the binary's allocator (spisor_memory_allocate), so a test's
 * counting allocator reaches them; every block they answer is freed with spisor_memory_release.
 */

typedef enum spisor_read_outcome { SPISOR_READ_DONE, SPISOR_READ_FAILED, SPISOR_READ_OUT_OF_MEMORY } spisor_read_outcome;

/** spisor_read_file, telling the reader that ran out of memory apart from every other failure. */
uint8_t *spisor_read_file_reporting(const char *path, size_t *length, spisor_read_outcome *outcome);

/** spisor_decode_utf8_lossy through the binary's allocator. */
uint8_t *spisor_decode_utf8_lossy_reading(const uint8_t *bytes, size_t length, size_t *out_length);

/** spisor_strip_comment_lines through the binary's allocator. */
char *spisor_strip_comment_lines_reading(const char *text, size_t length, size_t *stripped_length);

#endif
