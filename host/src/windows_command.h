#ifndef HOST_WINDOWS_COMMAND_H
#define HOST_WINDOWS_COMMAND_H

#include "cJSON.h"

/** Whether binary is a batch file or script that only the command shell starts on Windows: neither .exe nor .com. */
int host_needs_command_shell(const char *binary);

/**
 * The one argument cmd.exe /d /s /c takes to run binary, a .cmd or .bat file, with arguments: the line
 * quoted whole, binary's shell characters escaped, and each argument quoted and escaped for both parses,
 * as cross-spawn escapes them. Freed with free; NULL when memory runs out.
 *
 * @implNote Escaped twice because the shell parses the line once and a batch file passing %* on parses it
 * again; escaped once, an argument holding a quote and & runs a command of its own (CVE-2024-27980). The
 * line must reach cmd.exe verbatim, never quoted again.
 */
char *host_command_shell_line(const char *binary, const cJSON *arguments);

#endif
