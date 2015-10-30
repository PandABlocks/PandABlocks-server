/* Configuration interface. */

/* This should be called in a separate thread for each configuration interface
 * socket connection.  This function will run until the given socket closes. */
error__t process_config_socket(int scon);


/* Opaque type used to represent a single connection to the configuration
 * server, supports the connection_result methods when performing a get. */
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
