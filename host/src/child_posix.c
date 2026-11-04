#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "host_child.h"
#include "host_service.h"
#include "host_output.h"

extern char **environ;

#define START_FIX "install the application again, or run forebay-host install --catalog <path or url>"

static volatile sig_atomic_t running_child = 0;
static volatile sig_atomic_t terminate_pending = 0;

static void pass_terminate_on(int signal_number) {
    (void)signal_number;
    if (running_child > 0) {
        kill((pid_t)running_child, SIGTERM);
    } else {
        terminate_pending = 1;
    }
}

static void hold_on(int signal_number) {
    (void)signal_number;
}

/** The handlers this process runs while its child does; the ones it had are kept in previous. */
static void take_stop_signals(struct sigaction previous[3]) {
    const int signals[3] = {SIGINT, SIGHUP, SIGTERM};
    for (int index = 0; index < 3; index++) {
        struct sigaction action;
        memset(&action, 0, sizeof action);
        action.sa_handler = signals[index] == SIGTERM ? pass_terminate_on : hold_on;
        sigemptyset(&action.sa_mask);
        sigaction(signals[index], &action, &previous[index]);
    }
}

static void restore_stop_signals(const struct sigaction previous[3]) {
    const int signals[3] = {SIGINT, SIGHUP, SIGTERM};
    for (int index = 0; index < 3; index++) {
        sigaction(signals[index], &previous[index], NULL);
    }
}

static char **argument_list(const char *executable, const cJSON *arguments) {
    char **list = calloc((size_t)cJSON_GetArraySize(arguments) + 2, sizeof *list);
    size_t index = 0;
    if (list != NULL) {
        list[index++] = (char *)executable;
    }
    for (const cJSON *argument = list == NULL || arguments == NULL ? NULL : arguments->child; argument != NULL;
         argument = argument->next) {
        list[index++] = argument->valuestring;
    }
    return list;
}

static void free_environment_list(char **list) {
    for (char **entry = list; entry != NULL && *entry != NULL; entry++) {
        free(*entry);
    }
    free(list);
}

static char **environment_list(const cJSON *environment) {
    char **list = calloc((size_t)cJSON_GetArraySize(environment) + 1, sizeof *list);
    size_t index = 0;
    for (const cJSON *entry = list == NULL ? NULL : environment->child; entry != NULL; entry = entry->next) {
        if (!cJSON_IsString(entry)) {
            continue;
        }
        char *line = malloc(strlen(entry->string) + strlen(entry->valuestring) + 2);
        if (line == NULL) {
            free_environment_list(list);
            return NULL;
        }
        strcpy(line, entry->string);
        strcat(line, "=");
        strcat(line, entry->valuestring);
        list[index++] = line;
    }
    return list;
}

static cJSON *start_refusal(const char *executable, int error) {
    const char *parts[4] = {executable, " could not be started: ", strerror(error), ""};
    return host_refusal_of(parts, 4, START_FIX);
}

static int exit_code_of(int status) {
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

static double now_milliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1000000.0;
}

static void pause_briefly(void) {
    struct timespec pause = {0, 50 * 1000000L};
    nanosleep(&pause, NULL);
}

/** Waits for child, ticking watch every interval while it runs; its exit code. */
static int wait_for(pid_t child, const host_child_watch *watch) {
    int ticking = watch != NULL && watch->tick != NULL;
    double ticked = now_milliseconds();
    int status = 0;
    for (;;) {
        pid_t waited = waitpid(child, &status, ticking ? WNOHANG : 0);
        if (waited == child) {
            return exit_code_of(status);
        }
        if (waited < 0 && errno != EINTR) {
            return 1;
        }
        if (ticking && now_milliseconds() - ticked >= watch->interval_milliseconds) {
            watch->tick(watch->context);
            ticked = now_milliseconds();
        }
        if (ticking) {
            pause_briefly();
        }
    }
}

int host_run_child(const char *executable, const cJSON *arguments, const cJSON *environment, const host_child_watch *watch,
                   int *exit_code, cJSON **refusal) {
    *refusal = NULL;
    char **list = argument_list(executable, arguments);
    char **variables = environment == NULL ? NULL : environment_list(environment);
    if (list == NULL || (environment != NULL && variables == NULL)) {
        free(list);
        free_environment_list(variables);
        return 0;
    }
    struct sigaction previous[3];
    terminate_pending = 0;
    take_stop_signals(previous);
    pid_t child = 0;
    int error = posix_spawn(&child, executable, NULL, NULL, list, variables == NULL ? environ : variables);
    free(list);
    free_environment_list(variables);
    int ran = error == 0;
    if (error != 0) {
        *refusal = start_refusal(executable, error);
    } else {
        running_child = (sig_atomic_t)child;
        if (terminate_pending) {
            kill(child, SIGTERM);
        }
        if (watch != NULL && watch->started != NULL) {
            watch->started((double)child, watch->context);
        }
        *exit_code = wait_for(child, watch);
        running_child = 0;
    }
    restore_stop_signals(previous);
    return ran;
}

cJSON *host_replace_process(const char *executable, const cJSON *arguments) {
    char **list = argument_list(executable, arguments);
    if (list == NULL) {
        return NULL;
    }
    execv(executable, list);
    int error = errno;
    free(list);
    return start_refusal(executable, error);
}
