#ifndef RUNTIME_LOADER_ISSUES_H
#define RUNTIME_LOADER_ISSUES_H

#include "cJSON.h"

/** The issues found so far; complete drops to 0 when memory runs out, and every later add is skipped. */
typedef struct issue_list {
    cJSON *issues;
    int complete;
} issue_list;

void runtime_loader_add_issue(issue_list *list, const char *path, const char *detail, const char *fix);

/** Adds an issue for each thing wrong with the optional proxy and entry an application declares. */
void runtime_loader_check_application_facts(issue_list *list, const cJSON *application);

#endif
