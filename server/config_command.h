/* Interface for configuration commands. */

/* All configuration commands return a constant error message on failure or NULL
 * on success. */
typedef const char *command_error_t;


/* Uniform interface to entity and system commands. */
struct config_command_set {
    /* Implements name=value command.  If the value extents over multiple lines
     * then the rest of the value can be read from stream, otherwise this must
     * be left alone.  As this reads from the input stream this reading process
     * can fail. */
    bool (*put)(
        const char *name, const char *value, FILE *stream,
        command_error_t *error);

    /* Implements name? command.  If the result extends over multiple lines then
     * *multiline is set to a non NULL value and get_more() must be called to
     * read the remaining lines. */
    command_error_t (*get)(
        const char *name, char result[], size_t result_length,
        void **multiline);

    /* Implements process for reading the remaining lines from a multi-line get
     * which set *multiline. */
    bool (*get_more)(
        void *multiline, char result[], size_t result_length);
};


/* Top level implementation of name? and name=value commands. */
extern const struct config_command_set entity_commands;
/* Top level implementation of *name? and *name=value commands. */
extern const struct config_command_set system_commands;
