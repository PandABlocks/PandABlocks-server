/* Interface for system configuration commands. */

/* Top level implementation of *name? and *name=value commands. */
extern const struct config_command_set system_commands;

/* This is called during system startup. */
error__t initialise_system_command(void);

/* Releases any resources managed by this module. */
void terminate_system_command(void);
