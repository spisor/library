#ifndef RUNTIME_THREAD_H
#define RUNTIME_THREAD_H

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#ifdef _WIN32
typedef struct runtime_mutex {
    SRWLOCK lock;
} runtime_mutex;

typedef struct runtime_condition {
    CONDITION_VARIABLE condition;
} runtime_condition;

#define RUNTIME_MUTEX_INIT {SRWLOCK_INIT}
#define RUNTIME_CONDITION_INIT {CONDITION_VARIABLE_INIT}
#else
typedef struct runtime_mutex {
    pthread_mutex_t lock;
} runtime_mutex;

typedef struct runtime_condition {
    pthread_cond_t condition;
} runtime_condition;

#define RUNTIME_MUTEX_INIT {PTHREAD_MUTEX_INITIALIZER}
#define RUNTIME_CONDITION_INIT {PTHREAD_COND_INITIALIZER}
#endif

/** A thread runtime started, joined exactly once with runtime_thread_join. */
typedef struct runtime_thread {
#ifdef _WIN32
    HANDLE handle;
#else
    pthread_t handle;
#endif
    void (*run)(void *argument);
    void *argument;
} runtime_thread;

void runtime_mutex_lock(runtime_mutex *mutex);
void runtime_mutex_unlock(runtime_mutex *mutex);

/** Waits on `condition`, `mutex` held by the caller and held again on return; a wake may be spurious. */
void runtime_condition_wait(runtime_condition *condition, runtime_mutex *mutex);
void runtime_condition_wake_all(runtime_condition *condition);

/** Starts `run(argument)` on a new thread `thread` holds, which lives until the join; 0 when none could start. */
int runtime_thread_start(runtime_thread *thread, void (*run)(void *argument), void *argument);

/** Waits until the thread's run returned, and frees what the thread held. */
void runtime_thread_join(runtime_thread *thread);

#endif
