/* Interface for configuration commands. */


struct config_connection;


/* All configuration commands return a constant error message on failure or NULL
 * on success. */
typedef const char *command_error_t;

/* All result strings must be bounded by this limit. */
#define MAX_VALUE_LENGTH    256


/* Uniform interface to entity and system commands. */
struct config_command_set {
    /* Implements name=value command.  If the value extents over multiple lines
     * then the rest of the value can be read from the connection stream,
     * otherwise this must be left alone.  As this reads from the input stream
     * this reading process can fail. */
    bool (*put)(
        struct config_connection *connection,
        const char *name, const char *value, command_error_t *error);

    /* Implements name? command.  If the result extends over multiple lines then
     * *multiline is set to a non NULL value and get_more() must be called to
     * read the remaining lines. */
    void (*get)(
        struct config_connection *connection,
        const char *name, char result[],
        command_error_t *error, void **multiline);

    /* Implements process for reading the remaining lines from a multi-line get
     * which set *multiline. */
    bool (*get_more)(
        struct config_connection *connection, void *multiline, char result[]);
};


/* Top level implementation of name? and name=value commands. */
extern const struct config_command_set entity_commands;
/* Top level implementation of *name? and *name=value commands. */
extern const struct config_command_set system_commands;
