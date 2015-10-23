/* Interface for configuration commands. */


/* All result strings must be bounded by this limit. */
#define MAX_VALUE_LENGTH    256

struct config_connection;


/* Each of the configuration interface commands can return two errors: one
 * arising from communication with the channel, the other from processing the
 * command.  These errors need to be handled separately: the channel errors need
 * to be logged and will cause the channel to disconnect, whereas the command
 * errors need to be reported back to the client.
 *    To help make the distinction between these two errors a little clearer, we
 * use a new typedef for the command errors here. */
typedef error__t command_error_t;


/* Implements name=value command.  If the value extents over multiple lines then
 * the rest of the value can be read from the connection stream, otherwise this
 * must be left alone.  As this reads from the input stream this reading process
 * can fail. */
typedef error__t config_command_put_t(
    struct config_connection *connection,
    const char *name, const char *value, command_error_t *command_error);

/* Implements name? command.  If the result extends over multiple lines then
 * the first lines is returned, *multiline is set to a non NULL value, and
 * get_more() must be called to read the remaining lines. */
typedef void config_command_get_t(
    struct config_connection *connection,
    const char *name, char result[],
    command_error_t *command_error, void **multiline);

/* Implements process for reading the remaining lines from a multi-line get
 * which set *multiline.  Returns false when there are no more lines to read. */
typedef bool config_command_get_more_t(void *multiline, char result[]);

/* Uniform interface to entity and system commands. */
struct config_command_set {
    config_command_put_t *put;
    config_command_get_t *get;
    config_command_get_more_t *get_more;
};


/* Top level implementation of name? and name=value commands. */
extern const struct config_command_set entity_commands;
/* Top level implementation of *name? and *name=value commands. */
extern const struct config_command_set system_commands;

/* This is called during system startup. */
error__t initialise_config_command(void);
