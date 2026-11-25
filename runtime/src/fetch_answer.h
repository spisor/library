#ifndef RUNTIME_FETCH_ANSWER_H
#define RUNTIME_FETCH_ANSWER_H

#include "naula/transport.h"

/** A transfer the deadline ended, which naula answers CUT_OFF with nothing declared and its own detail and fix. */
static inline int runtime_fetch_is_deadline_cut_off(const naula_fetch_answer *answer) {
    return answer->outcome == NAULA_FETCH_CUT_OFF && answer->declared == 0 && answer->fix != NULL;
}

#endif
