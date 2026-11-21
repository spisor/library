#ifndef RUNTIME_ATOMIC_H
#define RUNTIME_ATOMIC_H

/** Adds one to `counter` from any thread, answering the new count. */
long runtime_atomic_increment(volatile long *counter);

/** Takes one from `counter` from any thread, answering the new count. */
long runtime_atomic_decrement(volatile long *counter);

#endif
