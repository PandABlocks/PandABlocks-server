/* Support for field access. */

struct put_table_writer;
struct connection_result;


struct class_context {
    const struct field *field;
    unsigned int number;
    const struct field_type *type;
    void *type_data;
    struct config_connection *connection;
};

/* The field_access is an abstract interface implementing access functions. */
struct class_access {
    /*  block[n].field?
     * Implements reading from a field. */
    error__t (*get)(
        const struct class_context *context,
        const struct connection_result *result);

    /*  block[n].field=value
     * Implements writing to the field. */
    error__t (*put)(const struct class_context *context, const char *value);

    /*  block[n].field<
     * Implements writing to a table, for the one class which support this. */
    error__t (*put_table)(
        const struct class_context *context,
        bool append, const struct put_table_writer *writer);
};


struct field_class;

const struct class_access *get_class_access(const struct field_class *class);
const char *get_class_name(const struct field_class *class);

/* Called during initialisation: returns class implementation with corresponding
 * name. */
error__t lookup_class(const char *name, const struct field_class **class);

/* Called during initialisation to add multiplexer indices for the named field.
 * Fails if the class doesn't support this operation. */
error__t class_add_indices(
    const struct field_class *class,
    const char *block_name, const char *field_name,
    unsigned int count, unsigned int indices[]);

// error__t initialise_class(
//     struct field *field, const struct field_class *class);


error__t initialise_classes(void);
