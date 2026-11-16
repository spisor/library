#ifndef SPISOR_FILE_READING_H
#define SPISOR_FILE_READING_H

#include <stddef.h>
#include <stdint.h>

typedef enum spisor_read_outcome { SPISOR_READ_DONE, SPISOR_READ_FAILED, SPISOR_READ_OUT_OF_MEMORY } spisor_read_outcome;

/** spisor_read_file, telling the reader that ran out of memory apart from every other failure. */
uint8_t *spisor_read_file_reporting(const char *path, size_t *length, spisor_read_outcome *outcome);

#endif
