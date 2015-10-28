/* Interface for configuration commands. */


/* All result strings must be bounded by this limit. */
#define MAX_VALUE_LENGTH    256

struct config_connection;


/* This structure is used by get to communicate its results back to the server.
 * Either a single value is written, or a multi-line result.  Only one of either
 * .write_one or .write_many may be called. */
struct connection_result {
    /* If this is called then it must be called exactly once. */
    void (*write_one)(
        struct config_connection *connection, const char *result);
    /* This can be called repeatedly (or not at all) if .write_one was not
     * called, but in this case .write_many_end() MUST be called at the end. */
    void (*write_many)(
        struct config_connection *connection, const char *result);
    /* If write_many() was called this is called to signal the end of writes. */
    void (*write_many_end)(struct config_connection *connection);
};


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
        struct connection_result *result);

    /* Implements name=value command. */
    error__t (*put)(
        struct config_connection *connection,
        const char *name, const char *value);

    /* Implements name< command.  This implements writing an array of values via
     * the returned put_table_writer interface. */
    error__t (*put_table)(
        struct config_connection *connection, const char *name, bool append,
        struct put_table_writer *writer);
};


/* Top level implementation of name? and name=value commands. */
extern const struct config_command_set entity_commands;
