#ifndef RUNTIME_PROCESS_ALIVE_H
#define RUNTIME_PROCESS_ALIVE_H

/**
 * Whether a process with this id is running, as Node's process.kill(pid, 0) tells it: a process this
 * one may not signal still counts as alive.
 *
 * @implNote An id that is not a whole number above zero is never alive, where Node would signal a
 * process group for zero or a negative id.
 */
int runtime_process_is_alive(double pid);

#endif
