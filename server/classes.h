/* Support for field access. */

struct put_table_writer;
struct connection_result;
struct field;

struct class_data;



/* This is passed to all the accessor methods below. */
struct class_context {
    unsigned int number;
    struct config_connection *connection;
    struct class_data *class_data;
};

struct class_attr_context {
    unsigned int number;
    struct config_connection *connection;
    struct class_data *class_data;
    const struct attr *attr;
};


/*  block[n].field?
 * Implements reading from a field. */
error__t class_get(
    const struct class_context *context,
    const struct connection_result *result);

/*  block[n].field=value
 * Implements writing to the field. */
error__t class_put(const struct class_context *context, const char *value);

/*  block[n].field<
 * Implements writing to a table, for the one class which support this. */
error__t class_put_table(
    const struct class_context *context,
    bool append, const struct put_table_writer *writer);

/*  block.field.*?
 * Hands request down to the appropriate type. */
error__t class_attr_list_get(
    const struct class_data *class_data,
    struct config_connection *connection,
    const struct connection_result *result);

/*  block[n].field.attr?
 * Reads from field attribute. */
error__t class_attr_get(
    const struct class_attr_context *context,
    const struct connection_result *result);

/*  block[n].field.attr=value
 * Writes to field attribute. */
error__t class_attr_put(
    const struct class_attr_context *context, const char *value);


/* Returns name of class. */
const char *get_class_name(const struct class_data *class_data);

/* Returns name of type. */
const char *get_type_name(const struct class_data *class_data);

/* Returns true if configuration changes are to be reported for this class. */
bool is_config_class(const struct class_data *class_data);

/* Generates a value change report for the given field. */
void report_changed_value(
    const char *block_name, const char *field_name, unsigned int number,
    const struct class_data *class_data,
    struct config_connection *connection,
    const struct connection_result *result);

/* Looks up named attribute. */
error__t class_lookup_attr(
    const struct class_data *class_data, const char *name,
    const struct attr **attr);


/* Called during initialisation to add multiplexer indices for the named field.
 * Fails if the class doesn't support this operation. */
error__t class_add_indices(
    const struct class_data *class_data,
    const char *block_name, const char *field_name,
    unsigned int count, unsigned int indices[]);

error__t class_add_attribute_line(
    const struct class_data *class_data, const char *line);


/* Creates and initialises the class for the given field using the given type
 * name, and returns class data to be stored with the field. */
error__t create_class(
    struct field *field, unsigned int count,
    const char *class_name, const char *type_name,
    struct class_data **class);

/* This should be called during shutdown for each created class. */
void destroy_class(struct class_data *class_data);


error__t initialise_classes(void);

void terminate_classes(void);
