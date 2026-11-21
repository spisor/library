#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "check.h"
#include "spisor/allocation.h"
#include "../src/file_replacement.h"

/**
 * @file
 * The temporary name a replacement writes beside its target, asked for by two new threads in turn after the main
 * thread asked first: every thread must get a name no other thread got, or two writers of one file at once collide on
 * the exclusive create of the same temporary file.
 */

#define TARGET SPISOR_SCRATCH_PATH "/temporary-name-target.json"

static char *named[2];

static void name_on_this_thread(int slot) {
    named[slot] = spisor_temporary_path_for(TARGET);
}

#if defined(_WIN32)
static DWORD WINAPI run_slot(LPVOID slot) {
    name_on_this_thread((int)(size_t)slot);
    return 0;
}

static void name_on_a_new_thread(int slot) {
    HANDLE thread = CreateThread(NULL, 0, run_slot, (LPVOID)(size_t)slot, 0, NULL);
    CHECK(thread != NULL);
    if (thread != NULL) {
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    }
}
#else
static void *run_slot(void *slot) {
    name_on_this_thread((int)(size_t)slot);
    return NULL;
}

static void name_on_a_new_thread(int slot) {
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, run_slot, (void *)(size_t)slot) == 0);
    pthread_join(thread, NULL);
}
#endif

int main(void) {
    char *first = spisor_temporary_path_for(TARGET);
    name_on_a_new_thread(0);
    name_on_a_new_thread(1);
    CHECK(first != NULL && named[0] != NULL && named[1] != NULL);
    CHECK(first != NULL && named[0] != NULL && strcmp(first, named[0]) != 0);
    CHECK(named[0] != NULL && named[1] != NULL && strcmp(named[0], named[1]) != 0);
    spisor_memory_release(first);
    spisor_memory_release(named[0]);
    spisor_memory_release(named[1]);
    return check_summary("temporary_name_test");
}
