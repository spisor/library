#ifndef RUNTIME_USER_HOME_H
#define RUNTIME_USER_HOME_H

#include "cJSON.h"

#define RUNTIME_NO_USER_HOME_MESSAGE \
    "the environment carries neither HOME nor USERPROFILE, so the user's home cannot be found"
#define RUNTIME_NO_USER_HOME_FIX "send an environment that carries the user's home as HOME, or USERPROFILE on Windows"

/**
 * The user's home environment names: USERPROFILE, else HOME, on Windows; HOME elsewhere. NULL when that
 * names nothing or an empty string.
 *
 * @implNote On Windows USERPROFILE wins because Git Bash sets HOME to an MSYS path the runtime cannot
 * use. There is deliberately no fallback to the process's own home: a caller that controls the
 * environment (a test, a service) must not silently get the real user's.
 */
const char *runtime_user_home_of(const cJSON *environment);

/** The string environment names under name, matched exactly; NULL when absent or no string. */
const char *runtime_environment_text(const cJSON *environment, const char *name);

/** Whether left and right are equal when ASCII letters are compared ignoring case, as Windows compares names. */
int runtime_equal_ignoring_ascii_case(const char *left, const char *right);

/**
 * The string environment holds under name as the OS named by windows matches variable names, or NULL.
 *
 * @implNote Windows environment names are case-insensitive, so Path and PATH are the same variable
 * there; the first entry that matches answers, as Object.keys(environment).find does.
 */
const char *runtime_environment_variable(const cJSON *environment, const char *name, int windows);

#endif
