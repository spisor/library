#ifndef HOST_TERMINAL_QUESTION_H
#define HOST_TERMINAL_QUESTION_H

/** Where a question is asked: the file read for the answer and the file the question is written to. */
typedef struct host_terminal {
    const char *input;
    const char *output;
} host_terminal;

/** The controlling terminal of this OS: /dev/tty, or CONIN$ and CONOUT$ on Windows. */
host_terminal host_controlling_terminal(void);

/**
 * Asks question on terminal and answers the first line typed, trimmed, freed with free; NULL when there is
 * no terminal to open or it ends before a line.
 *
 * @implNote Never reads standard input: under curl | sh that is the script itself. Each call opens and
 * closes the terminal, so nothing is held between questions.
 */
char *host_ask_on_terminal(const char *question, const host_terminal *terminal);

#endif
