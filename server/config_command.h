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


/* This structure is used by get to communicate its results back to the server.
 * Either a single value is written, or a multi-line result.  Only one of these
 * two methods may be called. */
struct connection_result {
    /* If this is called then it must be called exactly once. */
    error__t (*write_one)(
        struct config_connection *connection, const char *result);
    /* This can be called repeatedly, but the last call must be made with last
     * set to false -- the value is ignored for this last value. */
    error__t (*write_many)(
        struct config_connection *connection, const char *result, bool last);
};



/* Uniform interface to entity and system commands. */
struct config_command_set {
    /* Implements name? command.  All results are returned through the
     * connection_result interface, and any communication error is returned
     * through *comms_error.
     *    Note that ERROR_OK must be returned precisely if either
     * connection_result method was called, otherwise an error. */
    command_error_t (*get)(
        struct config_connection *connection, const char *name,
        struct connection_result *result, error__t *comms_error);

    /* Implements name=value command. */
    command_error_t (*put)(
        struct config_connection *connection,
        const char *name, const char *value);

    /* Implements name<format command.  This implements writing a multi-line
     * value, the rest of the data is read from the connection stream, and any
     * error arising is written to comms_error. */
    command_error_t (*put_table)(
        struct config_connection *connection,
        const char *name, const char *format, error__t *comms_error);
};


/* Top level implementation of name? and name=value commands. */
extern const struct config_command_set entity_commands;
