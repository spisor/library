#ifndef HOST_APPLICATION_CHOICES_H
#define HOST_APPLICATION_CHOICES_H

#include "cJSON.h"

/** Asks one question and answers what was typed, freed with free, or NULL when nothing can be asked. */
typedef char *(*host_question)(const char *question, void *context);

/**
 * The method chosen for each application of missing, the [{application, methods}] install.ecosystem
 * answered, as {id: method}: a number picks that method or skip, a method's name picks it, an empty
 * answer skips, and three answers that are neither skip too. Once ask answers NULL no further application
 * is asked about. NULL when memory runs out.
 */
cJSON *host_choose_methods(const cJSON *missing, host_question ask, void *context);

/** The question asked for application id offered the methods, the last choice skip, freed with free. */
char *host_method_question(const char *id, const cJSON *methods);

#endif
