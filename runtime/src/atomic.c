#include "atomic.h"

#ifdef _WIN32
#include <windows.h>

long runtime_atomic_increment(volatile long *counter) {
    return InterlockedIncrement(counter);
}

long runtime_atomic_decrement(volatile long *counter) {
    return InterlockedDecrement(counter);
}

#else

long runtime_atomic_increment(volatile long *counter) {
    return __atomic_add_fetch(counter, 1, __ATOMIC_ACQ_REL);
}

long runtime_atomic_decrement(volatile long *counter) {
    return __atomic_sub_fetch(counter, 1, __ATOMIC_ACQ_REL);
}

#endif
