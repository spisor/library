#ifndef HOST_OWN_EXECUTABLE_H
#define HOST_OWN_EXECUTABLE_H

/** The path of this process's own executable, freed with free; NULL when the OS will not say. */
char *host_own_executable(void);

#endif
