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
 * Whether this process's standard input and output are both a terminal, which a terminal interface draws on.
 *
 * @implNote Defined in terminal_attached.c, whose test build also answers 1 when SPISOR_TEST_TERMINAL_ATTACHED is 1, so
 * a test can take the terminal path on Windows, where it cannot hand a child a console it reads as attached; nothing a
 * user passes reaches the release build's.
 */
int host_terminal_attached(void);

/**
 * Asks question on terminal and answers the first line typed, trimmed, freed; NULL when there is
 * no terminal to open or it ends before a line.
 *
 * @implNote Never reads standard input: under curl | sh that is the script itself. Each call opens and
 * closes the terminal, so nothing is held between questions.
 */
char *host_ask_on_terminal(const char *question, const host_terminal *terminal);

#endif
