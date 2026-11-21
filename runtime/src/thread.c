#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "thread.h"

#ifdef _WIN32

void runtime_mutex_lock(runtime_mutex *mutex) {
    AcquireSRWLockExclusive(&mutex->lock);
}

void runtime_mutex_unlock(runtime_mutex *mutex) {
    ReleaseSRWLockExclusive(&mutex->lock);
}

void runtime_condition_wait(runtime_condition *condition, runtime_mutex *mutex) {
    SleepConditionVariableSRW(&condition->condition, &mutex->lock, INFINITE, 0);
}

void runtime_condition_wake_all(runtime_condition *condition) {
    WakeAllConditionVariable(&condition->condition);
}

static DWORD WINAPI run_thread(LPVOID started) {
    runtime_thread *thread = started;
    thread->run(thread->argument);
    return 0;
}

int runtime_thread_start(runtime_thread *thread, void (*run)(void *argument), void *argument) {
    thread->run = run;
    thread->argument = argument;
    thread->handle = CreateThread(NULL, 0, run_thread, thread, 0, NULL);
    return thread->handle != NULL;
}

void runtime_thread_join(runtime_thread *thread) {
    WaitForSingleObject(thread->handle, INFINITE);
    CloseHandle(thread->handle);
}

long runtime_atomic_increment(volatile long *counter) {
    return InterlockedIncrement(counter);
}

long runtime_atomic_decrement(volatile long *counter) {
    return InterlockedDecrement(counter);
}

#else

void runtime_mutex_lock(runtime_mutex *mutex) {
    pthread_mutex_lock(&mutex->lock);
}

void runtime_mutex_unlock(runtime_mutex *mutex) {
    pthread_mutex_unlock(&mutex->lock);
}

void runtime_condition_wait(runtime_condition *condition, runtime_mutex *mutex) {
    pthread_cond_wait(&condition->condition, &mutex->lock);
}

void runtime_condition_wake_all(runtime_condition *condition) {
    pthread_cond_broadcast(&condition->condition);
}

static void *run_thread(void *started) {
    runtime_thread *thread = started;
    thread->run(thread->argument);
    return NULL;
}

int runtime_thread_start(runtime_thread *thread, void (*run)(void *argument), void *argument) {
    thread->run = run;
    thread->argument = argument;
    return pthread_create(&thread->handle, NULL, run_thread, thread) == 0;
}

void runtime_thread_join(runtime_thread *thread) {
    pthread_join(thread->handle, NULL);
}

long runtime_atomic_increment(volatile long *counter) {
    return __atomic_add_fetch(counter, 1, __ATOMIC_ACQ_REL);
}

long runtime_atomic_decrement(volatile long *counter) {
    return __atomic_sub_fetch(counter, 1, __ATOMIC_ACQ_REL);
}

#endif
