#ifndef RUNTIME_SHIM_FORMAT_H
#define RUNTIME_SHIM_FORMAT_H

/**
 * The first line of every Windows <name>.shim file. The lines after it are the target's path and
 * then each fixed argument, every line ended by \n and written in UTF-8; no line can hold a line
 * break, since commands refuses a target or an argument that does.
 */
#define RUNTIME_SHIM_FORMAT "spisor-shim 1"

#endif
