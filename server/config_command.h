/* Interface for configuration commands. */


/* All result strings must be bounded by this limit. */
#define MAX_VALUE_LENGTH    256

struct config_connection;


/* This structure is used by get to communicate its results back to the server.
 * Either a single value is written, or a multi-line result.  Only one of these
 * two methods may be called. */
struct connection_result {
    /* If this is called then it must be called exactly once. */
    void (*write_one)(
        struct config_connection *connection, const char *result);
    /* This can be called repeatedly, but the last call must be made with last
     * set to false -- the value is ignored for this last value. */
    void (*write_many)(
        struct config_connection *connection, const char *result, bool last);
};


/* This is filled in by a successful call to put_table.  The the returned
 * .write() method should be called repeatedly until all data has been written
 * (length counts the number of data items, not bytes) or an error occurs, and
 * then .close() *must* be called. */
struct put_table_writer {
    void *context;
    error__t (*write)(
        void *context, const unsigned int data[], size_t length);
    void (*close)(void *context);
};


/* Uniform interface to entity and system commands. */
struct config_command_set {
    /* Implements name? command.  All results are returned through the
     * connection_result interface, and any communication error is returned
     * through *comms_error.
     *    Note that ERROR_OK must be returned precisely if either
     * connection_result method was called, otherwise an error. */
    error__t (*get)(
        struct config_connection *connection, const char *name,
        struct connection_result *result);

    /* Implements name=value command. */
    error__t (*put)(
        struct config_connection *connection,
        const char *name, const char *value);

    /* Implements name< command.  This implements writing an array of values. */
    error__t (*put_table)(
        struct config_connection *connection, const char *name, bool append,
        struct put_table_writer *writer);
};


/* Top level implementation of name? and name=value commands. */
extern const struct config_command_set entity_commands;
