/* Interface for configuration commands. */




/* These are defined in config_server.h. */
struct config_connection;
struct connection_result;


/* This is filled in by a successful call to put_table. */
struct put_table_writer {
    void *context;
    /* Call this repeatedly with blocks of data (length counts number of data
     * items, not bytes). */
    error__t (*write)(void *context, const unsigned int data[], size_t length);
    /* This must be called when this writer is finished with. */
    void (*close)(void *context);
};


/* Uniform interface to entity and system commands. */
struct config_command_set {
    /* Implements name? command.  All results are returned through the
     * connection_result interface.
     *    Note that ERROR_OK must be returned precisely if either
     * connection_result method was called, otherwise there was an error. */
    error__t (*get)(
        struct config_connection *connection, const char *name,
        const struct connection_result *result);

    /* Implements name=value command. */
    error__t (*put)(
        struct config_connection *connection,
        const char *name, const char *value);

    /* Implements name< command.  This implements writing an array of values via
     * the returned put_table_writer interface. */
    error__t (*put_table)(
        struct config_connection *connection,
        const char *name, bool append,
        const struct put_table_writer *writer);
};


/* Top level implementation of name? and name=value commands. */
extern const struct config_command_set entity_commands;
