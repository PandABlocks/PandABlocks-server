/* Configuration interface. */

/* A couple of buffer size definitions. */
#define MAX_NAME_LENGTH     32
#define MAX_RESULT_LENGTH   256

/* This should be far too long for any reasonable command. */
#define MAX_LINE_LENGTH     1024



/* Opaque type used to represent a single connection to the configuration
 * server, supports the connection_result methods when performing a get. */
struct config_connection;


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Change set reporting. */

/* Reportable changes are grouped into several groups, each separately
 * reportable:  Normal configuration changes, two sets of live data updates, and
 * a polled readback change set. */
enum {
    CHANGE_IX_CONFIG,       // *CHANGES.CONFIG?     Configuration changes
    CHANGE_IX_BITS,         // *CHANGES.BITS?       Bit output changes
    CHANGE_IX_POSITION,     // *CHANGES.POSN?       Position output changes
    CHANGE_IX_READ,         // *CHANGES.READ?       Read register changes
    CHANGE_IX_ATTR,         // *CHANGES.ATTR?       Read attribute changes
    CHANGE_IX_TABLE,        // *CHANGES.TABLE?      Read table changes
    CHANGE_IX_METADATA,     // *CHANGES.METADATA?   Metadata attributes

    CHANGE_SET_SIZE,        // MUST be last entry in list of enums
};
enum change_set {
    CHANGES_NONE     = 0,
    CHANGES_CONFIG   = 1 << CHANGE_IX_CONFIG,
    CHANGES_BITS     = 1 << CHANGE_IX_BITS,
    CHANGES_POSITION = 1 << CHANGE_IX_POSITION,
    CHANGES_READ     = 1 << CHANGE_IX_READ,
    CHANGES_ATTR     = 1 << CHANGE_IX_ATTR,
    CHANGES_TABLE    = 1 << CHANGE_IX_TABLE,
    CHANGES_METADATA = 1 << CHANGE_IX_METADATA,

    CHANGES_ALL      = (1 << CHANGE_SET_SIZE) - 1,      // *CHANGES?
};

STATIC_COMPILE_ASSERT(CHANGES_ALL < 1 << CHANGE_SET_SIZE)


/* Used for change management for a single connection. */
struct change_set_context {
    uint64_t change_index[CHANGE_SET_SIZE];
};


/* Allocates and returns a fresh change index. */
uint64_t get_change_index(void);


/* For each of the four change sets any change is associated with an increment
 * of a global change_index.  Each connection maintains a list of the most
 * recent change index seen for each change set.  This call updates the change
 * index for each selected change set and returns the previous change index for
 * all change sets. */
uint64_t update_change_index(
    struct change_set_context *context,
    enum change_set change_set, uint64_t reported[]);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* A couple of helper routines for output formatting. */

error__t __attribute__((format(printf, 3, 4))) format_string(
    char result[], size_t length, const char *format, ...);

/* Formats double without leading spaces. */
error__t format_double(char result[], size_t length, double value);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


/* This is used to pass information about the connection to name= commands. */
struct connection_context {
    struct change_set_context *change_set_context;
};


/* Structure used to return response to name? command.  If an error code is not
 * returned either a result should be written to .string[:.length] and .response
 * set to RESPONSE_ONE, or else .write_many() should be called for each multiple
 * result and set .response to RESPONSE_MANY. */
struct connection_result {
    /* This can be passed to update_change_index() to get a change set. */
    struct change_set_context *change_set_context;
    /* To return a single result it should be formatted into this array and
     * .response set to RESPONSE_ONE.  Note that this string can safely be used
     * as a formatting buffer for write_many() instead. */
    char *string;
    size_t length;
    /* To return multiple results this function should be called for each result
     * and .response set to RESPONSE_MANY. */
    void *write_context;
    void (*write_many)(void *write_context, const char *string);
    enum { RESPONSE_ERROR, RESPONSE_ONE, RESPONSE_MANY } response;
};


#define format_one_result(result, format...) \
    ( format_string(result->string, result->length, format)  ?: \
      DO(result->response = RESPONSE_ONE))

#define format_many_result(result, format...) \
    do { \
        snprintf(result->string, result->length, format); \
        result->write_many(result->write_context, result->string); \
    } while (0)


/* This is filled in by a successful call to put_table. */
struct put_table_writer {
    void *context;

    /* Called to write a single line to the table target. */
    error__t (*write)(void *context, const char *line);
    /* This must be called when this writer is finished with.  If write_ok is
     * not true then the entire write is discarded. */
    void (*close)(void *context, bool write_ok);
};


/* Uniform interface to entity and system commands. */
struct config_command_set {
    /* Implements name? command.  All results are returned through the
     * connection_result interface.
     *    Note that ERROR_OK must be returned precisely if either
     * connection_result method was called, otherwise there was an error. */
    error__t (*get)(const char *name, struct connection_result *result);

    /* Implements name=value command. */
    error__t (*put)(
        struct connection_context *connection,
        const char *name, const char *value);

    /* Implements name< command.  This implements writing an array of values via
     * the returned put_table_writer interface. */
    error__t (*put_table)(
        const char *name, bool append, bool binary,
        struct put_table_writer *writer);
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Interface for calling command processing. */


struct table_read_line {
    void *context;
    bool (*read_line)(void *context, char line[], size_t length);
};

error__t process_put_table_command(
    const struct config_command_set *command_set,
    const struct table_read_line *table_read_line,
    const char *name, const char *extra);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* This can be called to enable logging of all commands. */
void set_config_server_verbosity(bool verbose);

/* This should be called in a separate thread for each configuration interface
 * socket connection.  This function will run until the given socket closes. */
error__t process_config_socket(int scon);
