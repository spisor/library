#ifndef HOST_COMMANDS_H
#define HOST_COMMANDS_H

/** Each runs one forebay-host command with the arguments after its name, answering the exit code. */
int host_command_install(int count, char **arguments);
int host_command_status(int count, char **arguments);
int host_command_update(int count, char **arguments);
int host_command_remove(int count, char **arguments);
int host_command_launch(int count, char **arguments);
int host_command_hook(int count, char **arguments);
int host_command_background(int count, char **arguments);
int host_command_action(int count, char **arguments);
int host_command_catalog_build(int count, char **arguments);
int host_command_marketplace_build(int count, char **arguments);

/** Runs the forebay-host command line argv after the program name, answering the exit code: 0 done, 1 refused, 2 misused. */
int host_run_command_line(int count, char **arguments);

#endif
