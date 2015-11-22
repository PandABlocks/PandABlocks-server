/* Configuration interface. */

/* A couple of buffer size definitions. */
#define MAX_NAME_LENGTH     32
#define MAX_RESULT_LENGTH   256


/* Opaque type used to represent a single connection to the configuration
 * server, supports the connection_result methods when performing a get. */
struct config_connection;


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Change set reporting. */

/* Reportable changes are grouped into several groups, each separately
 * reportable:  Normal configuration changes, two sets of live data updates, and
 * a polled readback change set. */
#define CHANGE_IX_CONFIG    0   // *CHANGES.CONFIG?     Configuration changes
#define CHANGE_IX_BITS      1   // *CHANGES.BITS?       Bit output changes
#define CHANGE_IX_POSITION  2   // *CHANGES.POSN?       Position output changes
#define CHANGE_IX_READ      3   // *CHANGES.READ?       Read register changes
#define CHANGE_IX_ATTR      4   // *CHANGES.ATTR?       Read attribute changes
enum change_set {
    CHANGES_NONE     = 0,
    CHANGES_CONFIG   = 1 << CHANGE_IX_CONFIG,
    CHANGES_BITS     = 1 << CHANGE_IX_BITS,
    CHANGES_POSITION = 1 << CHANGE_IX_POSITION,
    CHANGES_READ     = 1 << CHANGE_IX_READ,
    CHANGES_ATTR     = 1 << CHANGE_IX_ATTR,
    CHANGES_ALL =               // *CHANGES?            All changes
        CHANGES_CONFIG | CHANGES_BITS | CHANGES_POSITION | CHANGES_READ |
        CHANGES_ATTR,
};

#define CHANGE_SET_SIZE     5
STATIC_COMPILE_ASSERT(CHANGES_ALL < 1 << CHANGE_SET_SIZE)


/* Used for change management for a single connection. */
struct change_set_context;


/* For each of the four change sets any change is associated with an increment
 * of a global change_index.  Each connection maintains a list of the most
 * recent change index seen for each change set.  This call updates the change
 * index for each selected change set and returns the previous change index for
 * all change sets. */
void update_change_index(
    struct change_set_context *context,
    enum change_set change_set, uint64_t reported[]);

/* Resets change counters.  Same as calling update_change_index and discarding
 * the reported array. */
void reset_change_context(
    struct change_set_context *context, enum change_set change_set);



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


/* This is used to pass information about the connection to name= commands. */
struct connection_context {
    struct change_set_context *context;
};


/* This structure is used by get to communicate its results back to the server.
 * Either a single value is written, or a multi-line result.  Only one of either
 * .write_one or .write_many may be called. */
struct connection_result {
    struct change_set_context *context; // temporary
    /* This is filled in to be passed back through the methods here. */
    struct config_connection *connection;
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
    error__t (*get)(const char *name, const struct connection_result *result);

    /* Implements name=value command. */
    error__t (*put)(
        struct connection_context *connection,
        const char *name, const char *value);

    /* Implements name< command.  This implements writing an array of values via
     * the returned put_table_writer interface. */
    error__t (*put_table)(
        const char *name, bool append, struct put_table_writer *writer);
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* This should be called in a separate thread for each configuration interface
 * socket connection.  This function will run until the given socket closes. */
error__t process_config_socket(int scon);
